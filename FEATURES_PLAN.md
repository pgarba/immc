# mc-clone — Total Commander parity plan

Spec source: `extern_dont_commit/tc_command.txt` (TC shortcut reference, © Thomas Bigler)
Audited against: `src/panes.cpp` (key handler `app_panes_handle_key` @1624-1865, special-key gate
`app_key_is_special` @1600-1622), `src/panes.h`, `src/filesystem.{h,cpp}`, `src/config.{h,cpp}`,
`src/theme.{h,cpp}`, `src/main.cpp` (event dispatch @104-160).

**Decisions this plan assumes**
1. **TC-faithful bindings.** Where our MC-style key collides with the TC table, TC wins and the MC
   binding is dropped or moved. Migration is done up-front (Phase 0) so later phases land on the
   final keymap once.
2. **Scope = parity, archives included.** Everything implementable without a new third-party
   dependency. Pack/unpack/browse (Alt+F5, Alt+Shift+F5, Alt+F6, Alt+F9, Alt+Shift+F9) shipped by
   driving the installed CLI tools instead of linking an archive library — see §6.
3. Deliberate deviations from the table are listed in §5 with the reason.

Legend: `OK` matches · `PART` partial · `MISS` missing · `CONFLICT` we bind that key to something else ·
`OUT` out of scope.

---

## 0. Build status

| Phase | State | What landed |
|---|---|---|
| 0 keymap migration | done | TC bindings live; colliding MC keys moved or dropped |
| A marking | done | `Space` marks, `+`/`-` extend/shrink, `Num/` mark-undo stack, dir size on marking a dir |
| B clipboard | done | `Ctrl+C/X/V`, overwrite prompt, progress pipeline reuse, failed cuts stay for retry |
| C quick filter | done | `Ctrl+S` live filter over a `view` row map, `Ctrl+F11` executables only, `Ctrl+F12` preset |
| D navigation & panes | done | `Ctrl+←/→`, `Ctrl+↑`, `Ctrl+Shift+↓/T/W`, `Ctrl+,`, `Alt+F11/F12`, `Alt+↓`, `Alt+F8`, `Alt+F1/F2` |
| E views & dialogs | done | `Shift+F10`/`Menu` context menu, `Alt+F3` default app, `Ctrl+F1/F2` short/long, `Shift+F2` compare, `Ctrl+Q` quick view pane, `Alt+F10`/`Ctrl+F8` tree, `Ctrl+B` branch view |
| F file operations | done | `Shift+F4` new file + editor, `Shift+F5` copy in place, `Shift+F6` rename in place, `Shift+Ctrl+F5` link in other pane, `Ctrl+L` count marked, `Alt+Shift+Enter` dir sizes |
| G delete to trash | done | `trash` in config (default on), `Del`/`F8` via `gio trash`/`trash-put`, `Shift+Del`/`Shift+F8` direct, prompt says which |
| H big items | done except multi-rename | quick search (`AltGr+letter`), `Shift+F1` custom columns, `Ctrl+Shift+F8` tree panel and `Shift+Tab` split view. Multi-rename dropped by decision |
| I keymap re-audit | done | every `MISS` row of §2 re-checked against `app_panes_handle_key`; two had never landed: `Ctrl+P` (path into the command line) and `Ctrl+I` (switch to the target pane, freed by moving invert to `Num*`) |

Config now also persists `view_mode`, `sort_mode`, `show_hidden`, `filter_preset`, `trash` and `columns`.

**Archives (§6) closed the last missing binding**, and inside an archive `F5` copies the marked
members out into the other pane, as TC's does. Four rows stay partial, each a deliberate shape
difference rather than an unimplemented shortcut: `F3` toggles the quick view instead of opening the
internal viewer (`Enter` and Shift+F3 do that), `Ctrl+F1`/`Ctrl+F2` set the view mode instead of a
short/long quick view, `Ctrl+D` is unbound while the hotlist is on `Shift+F9`, and the mark helpers
can scope to files but the dirs-only variant is not reachable from the keypad.

**Verification harness.** `tests/test_panes.cpp` includes `src/panes.cpp`, so the file-static helpers
(filter matcher, the `view` row map, marking, mark-undo, tabs, clipboard path helpers, branch view,
`fs_list_tree`, `fs_touch`/`fs_symlink`/`fs_trash`, quick search, the column spec, the tree model, the
archive pane mode with its guards and operations including copy-out, the deferred-action table, and the
operation dialog with its destination rules) run headless — 440 checks in `test-panes` plus 220 in
`test-archive`: `cmake --build build -j4 && (cd build && ctest)`.
It caught real bugs: `?` was never handled in the glob matcher; `tree_toggle()` kept a `vector`
reference across an `insert()`, so the `expanded` flag was written to freed memory and the next press
duplicated the row's children; and every menu item that started a long operation ran it inside the
ImGui frame, which opens a second frame from inside the first and corrupts ImGui. Bindings are now
also driven through `app_panes_handle_key` itself, not only through the classifiers — that is the path
on which `Alt+F5` used to reach the plain copy handler. Layout is measurable headless too: one frame
of the pack dialog against a long path settles whether the text reaches past the window, so a clipped
dialog is a failing check rather than a screenshot someone has to notice. So is an ID collision: both
panes draw into one window, and the tab strip pushed the same ID for its add-tab `+` on both sides, so
hovering it raised ImGui's "visible items with conflicting ID" box. Sweeping a virtual mouse over the
rendered UI and reading ImGui's own conflict counter turns that into a check — ImGui only counts items
answering to the ID hovered last frame, which is exactly what a hover does.
Smoke run: `SDL_VIDEODRIVER=offscreen timeout 3 ./build/mc-clone`.

---

## 1. Verified current state

| Area | Where | Notes |
|---|---|---|
| Key dispatch | `main.cpp:104-160` | modal confirm → op prompt → path bar → search field → `app_key_is_special` → else ImGui (cmd line). Any `Ctrl`/`Alt` combo is claimed by the app, so `Ctrl+C/X/V/S/B/M/L/P/Q` are currently **silent no-ops** — free to bind. |
| Marks | `Pane::marked` = `std::vector<bool>` **parallel to `entries`** (`panes.h:31`) | helpers `marked_count/toggle_mark/mark_range/toggle_range/select_all_marks/clear_marks/invert_marks` @98-146. Index-coupled to `entries` — any filtering/branch view must map through an index vector, never reorder `entries`. |
| File ops | `run_on_targets` @442, `count_existing_targets` @467, `do_delete` @486, `prog_begin` @616, `run_on_targets_prog` @630, `do_copy_now` @651, `do_move_now` @662, `confirm_yes` @717, `run_pending` @731 | copy/move/delete already have overwrite-confirm + pumpable progress dialog; new ops should reuse this pipeline. |
| fs layer | `filesystem.h` | `fs_copy/move/delete_entry_progress`, `fs_count_tree`, `fs_read_preview`, `fs_hex_dump`. Sort modes `NAME/EXT/SIZE/DATE` — **no unsorted**. No symlink, no tree walk, no trash. |
| `FileEntry` | `filesystem.h:8-13` | `name[256]`, `mode`, `size`, `mtime` — **no path**, so branch view / compare need a path-carrying entry type. |
| Tabs / history | `panes.h:12-13` `MAX_TABS 8`, `MAX_DIR_HISTORY 32`; `tab_*` @255-277, `push_history/navigate_to/history_back/forward` @204-253 | tab strip rendered by `render_tab_bar` @825. |
| Windows | `render_quick_view` @1055 (floating child), `render_viewer` @1096, `render_output` @1119, `render_hotlist` @1131, `render_search` @1164, `render_confirm` @1216, `render_about` @1234, `render_properties` @1272, `render_progress_dialog` @530 | quick view is a *floating window*, not a pane replacement. |
| Menu bar | `app_panes_render` @1304-1347 | only `Commands` + `Help`; **no keyboard access** (F9/F10 do not reach it). |
| File list render | `render_file_list` @850-1053 | table for MID/FULL, `ImGui::Columns` for BRIEF; row backgrounds drawn manually via `GetWindowDrawList`. Right-click popup `ctxmenu` @1007-1048. |
| Config | `config.h` | persists window geom, font, theme, tabs. **Not persisted:** `view_mode`, `show_hidden`, sort mode. |
| Theme | `theme.h` `MCThemeColors` | has `dir/exec/marked_bg/cursor_bg/cursor_outline`; **no diff colors** (needed by compare). |

---

## 2. Gap matrix (every line of `tc_command.txt`)

### Function keys
| TC | Ours today | Status | Action |
|---|---|---|---|
| F1 help | `F1` | DONE | F1 → new `render_help()` key reference window |
| F2 refresh source dir | F2 = rename | see §5 | keep rename; refresh stays `Ctrl+R` |
| F3 view | F3 toggles floating quick view | PART | F3 → internal viewer (reuse `open_viewer`); quick view moves to `Ctrl+F1/F2`, `Ctrl+Q` |
| F4 edit | F4 terminal editor | OK | — |
| F5 copy / F6 move | ✓ dialog + progress | DONE | destination typed and confirmed before anything runs; inside an archive `F5` copies the members out and `F6` still refuses |
| F7 mkdir | ✓ dialog | DONE | name typed in the dialog, Enter creates |
| F8, Del delete | `F8`, `Del`, `Shift+Del` | DONE | bind plain `Del`; add trash-vs-direct config (§4.7) |
| F9 activate menu | `F9` | DONE | F9 → open menu bar / per-pane user menu |
| F10 activate menu | **F10 = quit** | CONFLICT | F10 → menu; quit becomes `Alt+F4` (+ keep `Alt+Q`) |
| Alt+F1 / Alt+F2 choose drive | `Alt+F1 / Alt+F2` | DONE | volumes popup from `/proc/mounts` |
| Alt+F3 internal/external viewer | `Alt+F3` | DONE | Alt+F3 → `xdg-open` on cursor entry |
| Alt+Shift+F3 plugin-less lister | — | OUT | no plugin system |
| Alt+F4 quit / minimize | `Alt+F4` | DONE | add `Alt+F4` |
| Alt+F5 / Alt+Shift+F5 pack | `Alt+F5`, `Alt+Shift+F5` | DONE | name, format, level, password and volumes asked in the pack dialog |
| Alt+F6 / Alt+F9 / Alt+Shift+F9 unpack, test | `Alt+F6`, `Alt+F9`, `Alt+Shift+F9` | DONE | into the other pane / into this one / CRC check |
| Alt+F7 search | `Alt+F7` | DONE | bind `Alt+F7` (keep Shift+F7 as alias) |
| Alt+F8 command-history popup | `Alt+F8` | DONE | history popup above the command line |
| Alt+F10 directory tree window | `Alt+F10` | DONE | tree popup, Enter jumps |
| Alt+F11 / Alt+F12 focus dir bar | `Alt+F11 / Alt+F12` | DONE | focus left/right path `InputText` |
| Alt+Shift+F11 buttonbar | no buttonbar | OUT | — |
| Alt+← / Alt+→ history | ✓ | OK | — |
| Alt+↓ visited-dirs list | `Alt+↓` | DONE | history popup → jump |
| Shift+F1 custom columns view | `Shift+F1` | DONE | low priority: columns/view popup |
| Shift+F2 compare directories | `Shift+F2` | DONE | read-only pane compare |
| Shift+F3 show only cursor file when several marked | **Shift+F3 = cycle sort** | CONFLICT | sort → `Ctrl+F3..F7`; Shift+F3 = view cursor file only |
| Shift+F4 new text file + edit | `Shift+F4` | DONE | create empty file, then F4 flow |
| Shift+F5 copy in same dir w/ rename | `Shift+F5` | DONE | duplicate + rename prompt |
| Shift+Ctrl+F5 create links in target dir | `Shift+Ctrl+F5` | DONE | `fs_symlink` |
| Shift+F6 rename in same dir | `Shift+F6` | DONE | move-with-rename in place |
| Shift+F8 delete | `Shift+F8` | DONE | bind Shift+F8 |
| Shift+F10 local menu | `Shift+F10` | DONE | Shift+F10 + `SDLK_MENU` open `ctxmenu` |
| Shift+Esc minimize to tray | `Shift+Esc` | DONE | `SDL_MinimizeWindow` |

### Marking
| TC | Ours | Status | Action |
|---|---|---|---|
| Num+ extend / Num- shrink | `+`/`-` = select-all / clear (when cmd empty) | CONFLICT | TC semantics: mark/unmark at cursor and step; select-all/clear move to `Ctrl+Num+`/`Ctrl+Num-` |
| Num* invert | `*` ✓ | OK | — |
| Num/ restore marks | `Num/` | DONE | snapshot stack of `marked` before each bulk change |
| Ctrl+Num+ all / Ctrl+Num- none | `Ctrl+Num+` | DONE | add Num forms; `Ctrl+U` freed for pane swap |
| Ctrl+Shift+Num+ files+dirs, Ctrl+Shift+Num- files-only, Shift+Num+/-/* dir nuances | `Ctrl+Shift+Num+` | PART | add `mark_scope(files\|dirs\|both)` to the mark helpers |
| Alt+Num+ / Alt+Num- mark/unmark same extension | `Alt+Num+ / Alt+Num-` | DONE | ext match on cursor entry |
| Ins mark | ✓ (marks + advances) | OK | — |
| Space mark (+ dir size sum) | Ctrl+Space only | CONFLICT | Space marks when cmd line empty, else types a space; Space on unmarked dir fills computed size into the size column |
| Ctrl+A select all | ✓ | OK | — |
| Shift+arrows / Shift+PgUp/PgDn range mark | ✓ | OK | — |

### Navigation, tabs, panes
| TC | Ours | Status | Action |
|---|---|---|---|
| ↤ / Ctrl+PgUp parent dir | Backspace ✓, **Ctrl+PgUp = prev tab** | CONFLICT | Ctrl+PgUp → parent; `\` left unbound (§5); tabs move to `Ctrl+Tab` |
| Ctrl+PgDn open dir under cursor | **Ctrl+PgDn = next tab** | CONFLICT | Ctrl+PgDn → open under cursor in this pane |
| Ctrl+← / Ctrl+→ open under cursor in other pane | `Ctrl+← / Ctrl+→` | DONE | cursor stays put |
| Ctrl+< root | `Ctrl+<` | DONE | `Ctrl+Shift+Comma` → `/` |
| Ctrl+I switch to target pane | alias of Tab | OK | — |
| Ctrl+R refresh | ✓ | OK | — |
| Ctrl+T new tab (active) | ✓ | OK | — |
| Ctrl+Shift+T new tab (inactive) | `Ctrl+Shift+T` | DONE | `tab_new(pane, activate=false)` |
| Ctrl+W close tab | ✓ | OK | — |
| Ctrl+Shift+W close all tabs | `Ctrl+Shift+W` | DONE | keep ≥1 tab |
| Ctrl+↹ / Ctrl+Shift+↹ next/prev tab | Ctrl+PgUp/PgDn | CONFLICT | move to Ctrl+Tab; keep Ctrl+1..9 |
| Ctrl+U swap directories | **Ctrl+U = clear marks** | CONFLICT | Ctrl+U swaps pane dirs; clear marks → Ctrl+Num- |
| Ctrl+Shift+U swap dirs **and** tabs | `Ctrl+Shift+U` | DONE | swap whole tab sets |
| Ctrl+↑ open cursor in new tab (same pane) | `Ctrl+↑` | DONE | |
| Ctrl+Shift+↓ open cursor in new tab (other pane) | `Ctrl+Shift+↓` | DONE | |
| Tab switch panes | ✓ | OK | — |
| Backspace → parent | ✓ | extra | keep (not in TC table, additive) |
| Home → first entry / Shift+Home → $HOME | ✓ | extra | keep |

### Views / dialogs
| TC | Ours | Status | Action |
|---|---|---|---|
| Ctrl+F1 quick view short / Ctrl+F2 quick view long | F3 floating quick view | PART | quick view bound to Ctrl+F1 (name+size) / Ctrl+F2 (full columns) |
| Ctrl+Q quick-view **pane** instead of a file pane | `Ctrl+Q` | DONE | replace the inactive pane's list with a quick view |
| Ctrl+F3..F6 sort name/ext/date/size, Ctrl+F7 unsorted | `Ctrl+F3..F6` | DONE | direct sort keys; add `FS_SORT_NONE` |
| Ctrl+F8 / Ctrl+Shift+F8 tree | `Ctrl+F8 / Ctrl+Shift+F8` | DONE | toggle the Alt+F10 tree (embedded later) |
| Ctrl+B branch view / Ctrl+Shift+B marked branch view | `Ctrl+B` | DONE | flat recursive listing in-pane |
| Ctrl+S quick filter / Ctrl+Shift+S re-apply / Ctrl+F10 clear | `Ctrl+S` | DONE | per-pane live filter |
| Ctrl+F11 executables-only / Ctrl+F12 user filter | `Ctrl+F11` | DONE | filter presets; `filter_presets` in config |
| Ctrl+D directory menu (hotlist) | Alt+D add, Shift+F9 list | PART | Ctrl+D opens the hotlist popup |
| Ctrl+L size of marked | `Ctrl+L` | DONE | reuse `fs_count_tree` |
| Alt+Shift+Enter sum all dir sizes | `Alt+Shift+Enter` | DONE | fill size column for dirs |
| Alt+Enter properties | ✓ | OK | — |
| Enter open/run/command | ✓ (cmd-first) | OK | — |
| AltGr+letter / Ctrl+Alt+letter quick search by name | `AltGr+letter / Ctrl+Alt+letter` | DONE | type-ahead jump in the pane |
| Shift+Enter `cmd /c` NOCLOSE | — | OUT | Windows-specific |

### Clipboard & rename
| TC | Ours | Status | Action |
|---|---|---|---|
| Ctrl+C copy to clipboard / Ctrl+X cut / Ctrl+V paste | `Ctrl+C` | DONE | app-level file clipboard (§4.4) |
| Ctrl+M multi-rename tool | unbound | dropped | user decision: not needed |
| Ctrl+P current path into command line | `Ctrl+P` appends it; `Alt+C` prefills `cd ` | OK | — |
| Ctrl+Z comments | — | OUT | no comment store |

### Out of scope (Windows/legacy or dependency-bound)
FTP (`Ctrl+F`, `Ctrl+Shift+F`, `Ctrl+N`, `Ctrl+Shift+M`) · thumbnails (`Ctrl+Shift+F1`) · comments
(`Ctrl+Z`, `Ctrl+Shift+F2`) · printing (`Ctrl+F9`) · NOCLOSE.PIF · buttonbar (`Alt+Shift+F11`) ·
drive letters (`Alt+F1/F2` → implemented as mount picker instead) · writing into RAR (no writer in
`7z`, so RAR is read-only) · archive *mutations* (rename/delete/move inside an archive).

**Count:** 74 bindings in the table — 58 bound (16 already matched at audit time, 42 implemented by
the phases below), 4 partial, 8 conflicts resolved by the Phase 0 migration, 4 out of scope.

---

## 3. Phase 0 — keymap migration (do first, ~1h)

Everything below is a rebind of behaviour that already exists; it clears the collisions the later
phases need. Touchpoints: `app_key_is_special` @1600, `app_panes_handle_key` @1624, menu labels
@1304-1347, `render_about` @1234.

| Key | Now | Becomes |
|---|---|---|
| F10 | quit | open menu bar (`ImGui::OpenPopup` on the menu, or a Commands popup) |
| Alt+F4 | — | quit (keep `Alt+Q`) |
| Ctrl+PgUp | prev tab | parent directory |
| Ctrl+PgDn | next tab | open entry under cursor in this pane |
| Ctrl+Tab / Ctrl+Shift+Tab | — | next / prev tab |
| Ctrl+U | clear marks | swap pane directories (clear marks → `Ctrl+Num-`) |
| Ctrl+Num+ / Ctrl+Num- | — | select all / clear marks |
| `+` / `-` | select all / clear | TC extend / shrink selection |
| Shift+F3 | cycle sort | view cursor file only |
| Ctrl+F3..F7 | — | sort name / ext / date / size / unsorted |

Acceptance: `render_about` text and menu shortcut hints match the new map; no dead key from the table
above.

---

## 4. Feature phases

### 4.1 Phase A — marking parity (~2h)
- `Space`: add `SDLK_SPACE` to `app_key_is_special` guarded by `app->cmd[0] == '\0'`; handler →
  `toggle_mark` (no auto-advance, unlike Ins).
- Plain `Del` → `request_delete`; add `Shift+F8` alongside `Shift+Del`.
- TC `+`/`-` semantics: new `extend_marks(Pane&, int dir)` = toggle at cursor then step cursor.
- `Num/` restore: `std::vector<std::vector<bool>> mark_undo` in `Pane`, pushed by every bulk op
  (select-all, clear, invert, ext-mark).
- Scope filter: `enum MarkScope { SCOPE_ALL, SCOPE_FILES, SCOPE_DIRS }` param on
  `select_all_marks/clear_marks/invert_marks`; wire the `Shift` variants.
- `Alt+Num+` / `Alt+Num-`: `ext_of(entries[selected])`, apply to non-dir entries.
- Dir-size-on-mark: `Pane::dir_size` cache keyed by entry index; Space on an unmarked dir runs
  `fs_count_tree` (async-guarded: cap at ~200k entries, show `…` while counting).
- Touchpoints: `panes.h` (new members), `panes.cpp:98-146`, `1751-1765`, `1791`.

### 4.2 Phase B — internal clipboard (~3h)
- New `struct FileClipboard { std::vector<std::string> paths; bool cut; }` in `AppPanes`;
  `marked_or_cursor(app, pane)` helper feeds it (reuse the target-collection logic in
  `count_targets` @460 / `run_on_targets` @442).
- `Ctrl+C` / `Ctrl+X` fill it; `Ctrl+V` → `do_paste(app)`: iterate paths into
  `active.current_dir`, pre-check collisions with a `count_existing_targets`-style pass →
  `CONF_OVERWRITE`, then run through `prog_begin`/`run_on_targets_prog` with `fs_copy_entry_progress`
  or `fs_move_entry_progress`. On a completed cut, clear the clipboard; on abort, keep the
  remaining entries (track the index).
- Status line: extend the bottom bar @1439-1456 with `Clipboard: N (copy|cut)`; also show marked
  count — both already derivable from `marked_count` @98.
- **Dispatch caveat (must handle):** `main.cpp` routes every `Ctrl` combo to the app, so text
  `Ctrl+C/X/V` inside the command line is currently dead anyway. Rule: cmd line empty → file
  clipboard; cmd line non-empty → forward the event to `ImGui_ImplSDL3_ProcessEvent` so text
  editing keeps working. Implement in `app_key_is_special` @1620.

### 4.3 Phase C — quick filter (~3h)
- `Pane` gets `char filter[256]`, `bool filter_active`, `char last_filter[256]`, and
  `std::vector<int> view` — index map into `entries`. **Never reorder or shrink `entries`**: marks,
  `selected`, and `sel_memory` are index-coupled. `render_file_list` iterates `view` instead of
  `0..entry_count`; keyboard nav maps `selected` through `view`.
- Matcher: case-insensitive substring; if the pattern contains `*`/`?`, glob-match the name.
- `Ctrl+S` open+focus filter, `Ctrl+Shift+S` re-apply `last_filter`, `Ctrl+F10` clear, `Esc` clear.
- Clear on directory change (`refresh_pane` @152) — TC behaviour.
- Filter row renders inside the pane between the path bar and the list (adjust the vertical budget
  math @1355-1362).
- Presets: `Ctrl+F11` executables-only (`is_executable`), `Ctrl+F12` first user preset; add
  `filter_presets[N]` to `MCConfig`.

### 4.4 Phase D — navigation & panes (~3h)
- `Ctrl+←/→` → `navigate_to(app, &other, path_of_cursor)`; `Ctrl+↑` → `tab_new` + navigate;
  `Ctrl+Shift+↓` → new tab in the other pane.
- `Ctrl+Shift+T` → `tab_new(pane, false)`; `Ctrl+Shift+W` → `tab_close_all` (floor at 1 tab).
- `Ctrl+U` swap dirs: exchange `current_dir` + reload; `Ctrl+Shift+U` also swaps the `tabs[]` arrays.
- `Ctrl+,`/`Ctrl+Shift+Comma` → `/`.
- `Alt+F11/F12` → set `active_path_pane` and `SetKeyboardFocusHere` on that pane's path `InputText`
  (needs a one-frame flag consumed in `app_panes_render` @1370/1400).
- `Alt+↓` history popup (reuse `history[]` @panes.h:36) and `Alt+F8` command-history popup
  (reuse `cmd_history[]`); both are small modal child windows like `render_hotlist` @1131.
- `Alt+F1/F2` volumes popup: parse `/proc/mounts` (skip pseudo-fs), pick → `navigate_to`.

### 4.5 Phase E — views & dialogs (~5h)
- `F1` → `render_help()`: full key table (single source of truth — generate from the same table used
  by the menu hints).
- `F9` menu access; `Shift+F10` + `SDLK_MENU` → open the existing `ctxmenu` popup at the cursor row
  (needs the row's screen pos retained from `render_file_list`).
- `Shift+Esc` → `SDL_MinimizeWindow` (window handle already stored as `AppPanes::sdl_window`).
- `Alt+F3` → `xdg-open` (reuse the spawn pattern in `spawn_terminal_editor` @788).
- `Ctrl+F1/F2` quick view short/long; `Ctrl+Q` quick-view **pane**: `Pane::quick_view_pane` flag,
  `render_file_list` swaps to a preview renderer for the inactive pane.
- `Alt+F10` / `Ctrl+F8` tree window: lazy child-counting tree over `Pane::history`-style cache,
  Enter → `navigate_to`.
- `Shift+F2` compare: mark `entries` of the active pane against the other pane's name→size map;
  add `diff_missing` / `diff_differ` to `MCThemeColors` (`theme.h`, all 7 themes in `theme.cpp`).
- `Ctrl+B` branch view: needs a path-carrying entry type — add
  `struct TreeEntry { std::string relpath; mode_t mode; off_t size; time_t mtime; }` and
  `fs_list_tree(root, out, max_depth, max_entries)` in `filesystem.cpp`; `Pane` gains
  `int list_mode` (LIST/BRANCH) and a `TreeEntry` vector; `Ctrl+Shift+B` walks only marked dirs.
  Enter on a branch row opens that dir in the pane and exits branch mode.

### 4.6 Phase F — file operations (~3h)
- `Shift+F4`: `fs_touch` empty file in `current_dir`, then `spawn_terminal_editor`.
- `Shift+F5`: copy cursor entry to `name.copy` in the same dir; `Shift+F6`: same-dir move/rename (the
  rename dialog, `fs_move_entry`).
- `Shift+Ctrl+F5`: `fs_symlink(src_abs, dst)` in the other pane.
- `Ctrl+L`: `fs_count_tree` over marked → status line.
- `Alt+Shift+Enter`: fill the size column for all dirs (bounded, cancellable with Esc).
- fs additions in `filesystem.{h,cpp}`: `fs_symlink`, `fs_touch`, `fs_list_tree`, `fs_trash`.

**The operation is the dialog, not the keystroke.** `F5`, `F6`, `F7`, `F2` and `Shift+F4` open one modal
dialog and touch nothing until it is answered. Copy and move prefill the `To:` field: one file prefills
its full target path, so editing the last segment renames while copying; several files, or members coming
out of an archive, prefill the other pane's directory, because one name for several sources would
silently drop all but one. A value that names an existing directory, or that ends in `/`, is a
destination directory — and that trailing slash is also the only way to say *make that folder first*, so a
typo in a typed path stays an error instead of creating a second directory beside the intended one.
Copying a file onto itself is refused with a message that says to type a different name, because the copy
opens the target for writing before it reads the source and the file would simply be lost. The sources are
captured when the dialog opens, into an `OpPlan`, so a click or a pane switch while the overwrite question
is up cannot change what the operation acts on; the rename and mkdir dialogs capture the folder, and for a
rename the entry on their own "Renaming:" line, for the same reason — and several entries cannot take one
name anyway. Enter commits and Esc abandons; a failure (`F7` on a name
already taken) leaves the dialog open with the reason in it, so the fix is typed in place instead of the
whole operation restarted. While it is up the arrows, `Home`, `End` and `Delete` belong to the text field
rather than the file list, and the OK button only records the request — the run happens between frames,
like every other long operation.

### 4.7 Phase G — delete-to-trash option (~1h)
- `MCConfig::trash` (default on when `gio` or `trash-put` is on PATH). `fs_delete_entry` routes
  through `gio trash` / `trash-put`, falling back to direct unlink. `Del`/`F8` = configured mode,
  `Shift+Del`/`Shift+F8` = always direct (matches TC's "direkt/Papierkorb" split).

### 4.8 Phase H — bigger items (each its own session)
- ~~`Ctrl+M` multi-rename tool~~ — **dropped by user decision**, not on the parity list for this project.
- Quick search (`AltGr+letter`) — **done**. SDL3 has no `SDL_KMOD_ALTGR`; AltGr arrives as
  `SDL_KMOD_MODE`, or as `SDL_KMOD_RALT` on layouts without a real AltGr, so the binding tests
  both. It is checked *before* the `Alt+letter` bindings, which is what keeps `AltGr+Q` a search
  instead of a quit. The word walks `view`, so a quick filter narrows it; a miss leaves the
  cursor alone; `Esc` or `Enter` ends it.
- `Shift+F1` custom columns — **done**. `columns` in config (comma-separated: name, ext, size,
  date, perms, type). Name is always first and cannot be turned off. The set drives the long
  view (`Ctrl+F2`); the mid view keeps the fixed name/size/date trio, brief stays name-only.
- Embedded/split tree panes (`Ctrl+Shift+F8`, `Shift+Tab`) — **done**. `Ctrl+Shift+F8` replaces the
  panel's file list with the tree; `Shift+Tab` splits the panel 40/60 into tree + list. Each pane
  owns its `TreeRows` model, and the floating `Alt+F10` window edits that same model, so the two
  views never disagree. In embed mode the keyboard drives the tree (arrows, Left/Right collapse and
  expand, Enter navigates); in split mode the keyboard stays with the file list and the tree is
  mouse-driven.

---

## 5. Deliberate deviations from `tc_command.txt`

| Table says | We keep | Why |
|---|---|---|
| `F2` = re-read source dir | `F2` = rename | the table's F2 is a Norton-Commander-ism; real TC uses F2 for rename, and our rename flow is built on it. Refresh stays `Ctrl+R` (also in the table). |
| `F10` = menu only | `F10` = menu (migrated), `Alt+F4`/`Alt+Q` = quit | TC-faithful; `Alt+Q` kept as an extra. |
| `Backspace` unbound | `Backspace` = parent dir | additive convenience; TC's `Ctrl+PgUp` and `\` do the same. `\` is left unbound: it is a legal filename byte on Linux, and `Ctrl+,` already covers TC's root jump. |
| `Ctrl+O`, `Alt+.`, `Ctrl+1..9`, `Alt+C`, `Alt+PgUp/PgDn` | kept | MC extras not in the TC table; no collisions after Phase 0. |
| `Ctrl+B` branch view is a full browser | branch view is capped (depth 6, 4000 rows) and `F2`/`F7` are refused inside it | the subtree is a snapshot; rename/mkdir address rows by name only, so they are blocked instead of silently hitting the wrong file. Delete/copy/move/view go through `entry_rel()` and are correct. |
| `Ctrl+I` = switch to the target pane | same, and invert moves to `Num*` | `Ctrl+I` was our invert shortcut. `Num*` is TC's own invert binding and was already wired, so `Ctrl+I` goes back to the pane switch the table asks for instead of staying a documented deviation. |
| `Ctrl+P` replaces the command line | appends to it | a path can be 4096 bytes and the command buffer is 512, and `rm ` + path is the common case; appending keeps both the typed verb and the path, and a too-long path truncates inside the buffer instead of overflowing it. |
| `Shift+F2` compares content | compares name + size + type | content compare needs a diff engine; size/type mismatch is the 95% case. Colors come from the new theme slots `diff_missing` / `diff_differ`. |
| `AltGr+letter` quick search | same, detected as `SDL_KMOD_MODE` or `SDL_KMOD_RALT` | SDL3 has no `SDL_KMOD_ALTGR`. The binding is tested before the `Alt+letter` handlers so `AltGr+Q` searches instead of quitting. |
| tree panel keyboard | embed mode: keyboard drives the tree; split mode: keyboard stays with the file list | in a split panel there is no third focus to cycle to, and `Tab` already switches panes in TC. The tree is mouse-driven in split mode. |
| archives use a plugin/packer dialog | CLI tools (`zip`/`unzip`, `tar`, `7z`) through `fork`/`execvp` | no third-party dependency is linked, and the shape matches the helpers we already shell out to (`gio trash`, `xdg-open`). The cost is that a format without its tool installed reports "No tool installed for … archives" instead of working. |
| unpack asks a dialog per file (path, overwrite, subdirs) | one `To:` folder in the copy dialog, and one overwrite question for the whole set | a modal per-file prompt inside the progress dialog is not implementable at this size, and a tool that prompts on a stdin it cannot answer would hang the operation. The destination is still asked for, and existing targets still get one question before `unzip -o` / `7z -y` / `tar` overwrite them. |
| rename/delete/move inside an archive | refused | a partial implementation would have to rewrite the archive to answer the shortcut. Extract, change, pack again is the TC workflow anyway, and the refusal says so. |
| RAR read/write | read, extract, test only | `7z` registers Rar and Rar5 decoders but has no writer, and there is no free RAR encoder to shell out to. |

---

## 6. Archives

No archive library is linked. `src/archive.{h,cpp}` drives the CLI tools that are already on the
machine — `zip`/`unzip`, `tar`, and `7z` — the same way the trash and "open with" features drive
`gio trash` and `xdg-open`. Formats follow the file suffix: `.zip`, `.tar`, `.tar.gz/.tgz`,
`.tar.bz2`, `.tar.xz`, `.7z`, and `.rar`/`.rar5` read-only through `7z`, which ships Rar and Rar5
decoders. There is no RAR writer, so RAR is browse/extract/test only.

**Members are argv, never shell text.** Every operation builds an `argv` array and runs it through
`fork`/`execvp`, so a member named `a file.txt`, `it's.txt` or `*.txt` behaves like any other. The
runner merges stdout and stderr, parses progress out of the tool's own lines (one per member for
`zip`/`unzip`/`tar`, percentages for `7z`), and kills the child when the progress dialog's Abort is
used — archive operations reuse the copy/move progress dialog and its callback signature unchanged.

**Two names per member.** `ArchEntry` keeps `rel` (normalised: no `./`, no trailing `/`) for display
and for the virtual tree, and `raw` (exactly what the tool listed) for the extraction argv. They
differ in real archives: `tar czf x.tgz -C dir .` stores `./a file.txt`. Passing the display name back
to `tar` would find nothing, so extraction always goes through `raw`.

**Browsing is a pane mode, not a parallel UI.** A pane's `current_dir` may be a *virtual* path —
`/home/adam/pkg.zip/sub` — and `arch_split_virtual()` finds the first component that is an existing
regular file with an archive suffix. Everything downstream (tabs, tab labels, directory history, the
path bar, the hotlist, filters, marks, selection memory, `Ctrl+Left/Right`, `Ctrl+Up`) therefore works
without knowing archives exist. A directory that merely *looks* like an archive is not a boundary:
only a regular file with an archive suffix is. `LIST_ARCHIVE` rows carry their level-relative path in
`Pane::rel`, which is what makes the virtual path compose; the tool-facing member name is that path
plus the pane's `archive_prefix`. An archive root lists a `..` row like any other directory, so Enter
leaves the archive; before that the only way out was `Backspace`, which is invisible to anyone who has
not read the help.

**Archive contents are read-only, and the guards say so.** Rename, mkdir, new file, delete, cut,
paste, copy-in-place, links, branch view, the tree panel, search, properties, directory sizes, the
editor and "open with the default application" all refuse inside an archive rather than resolving a
path that does not exist on disk. `F5` is the exception, and it is TC's: inside an archive it copies
the marked members, or the cursor row, **out** into a folder the copy dialog asks for, which defaults to
the other pane's directory. `F6` still refuses,
because a move would have to delete the member afterwards and no format we can write lets us do that.
The way to change an archive is the way it is in TC: extract, change, pack again. `Ctrl+P` puts the
*archive file* on the command line, never the virtual path.

**Operations.** `Alt+F5` packs the marks (or the cursor row) and opens a dialog; `Alt+Shift+F5` is
the same with "delete the sources afterwards" already ticked, which makes it a move into the archive.
`Alt+F6` extracts the archive under the cursor into the other pane's directory, `Alt+F9` into this
pane's (inside an archive, "this pane's" means the directory holding the archive file), `Alt+Shift+F9`
verifies every member's CRC without writing. `Ctrl+C` inside an archive puts members on the clipboard
with the archive as their context, and `Ctrl+V` in a real directory extracts exactly those; `Ctrl+X`
there refuses instead of pretending a member can be deleted. `Enter` on a member unpacks only that
member into a temporary directory and opens the viewer, which removes the directory when it closes.
`F5` inside an archive does the same without the clipboard: it extracts the marked members, or the
cursor row, into the folder the dialog names, the other pane's directory by default, and keeps each
member's path inside the archive, so two members called `inner.txt` in different folders of one archive
cannot land on each other. For an archive the field is always a folder and never a new name: renaming
one member of a set would silently drop the rest.

**A folder row asks for its subtree, and the tools disagree about what naming it means.** `unzip`
matches member names literally, so extracting `sub/` unpacks an empty directory and leaves the files
behind; `tar` recurses into `sub/` and then reports "Not found in archive" for every child named
alongside it, exiting non-zero *after* writing everything — which reads to the user as a failed copy
that actually worked. So `arch_extract_members()` expands a requested directory into its subtree's
leaves: its files, plus any directory under it that holds no files. Every tool writes those and
creates the parents on the way, an empty folder still comes out as a folder, and the per-file progress
counts what is actually written instead of one row per folder.

**Packing asks, and the answer has to be honest.** Packing used to ask for a name on one line in the
bottom bar, which left the format to whatever suffix the user happened to type. It now opens a dialog
offering the name, the format, a compression level, a password and a volume size, and the sources are
captured when it opens so marks cannot change underneath it. What the dialog offers is what the installed
tools can actually do, and nothing is quietly dropped:

| choice | zip | 7z | tar family |
|---|---|---|---|
| level 0-9 | `-0`..`-9` | `-mx0`..`-mx9` | `GZIP`/`BZIP2`/`XZ_OPT` in the child's environment |
| password | `-P` (argv) | `-p` (argv) | impossible — tar has no encryption |
| volumes | refused | `-v{bytes}`, reads back as one archive | refused |
| RAR | — | read-only: no writer exists on a normal system | — |

`arch_pack_supported()` is the single place that decides, and it is called both to grey out the dialog's
Pack button and again before running the tool, so the preview and the operation cannot disagree. A
format that cannot honour a choice is refused with the reason ("tar cannot encrypt; choose zip or 7z",
"only 7z writes volumes the tools can read back") rather than producing an archive that is not what was
asked for — a "password protected" archive that is not encrypted is worse than an error. A split `.zip`
is refused on purpose: `zip -s` writes `.z01` parts that `unzip` then warns about and mis-extracts.
The combo owns the suffix, so a name typed as `mismatched.zip` under a `tar.xz` format produces
`mismatched.tar.xz`, and the dialog shows the file name it will write. A password is cleared each time
the dialog opens — it is not a preference, and it should not end up in the next archive. The dialog
also says plainly that the password is handed to the tool, so it is visible in the process list for the
length of the pack; `zip -e`, which would prompt instead, needs a terminal a GUI process does not have.

**Two details that make volumes usable.** A split archive names its parts after the whole
(`movies.7z.001`), so `arch_kind_of()` drops a trailing all-digit component and asks again — otherwise
the pack dialog's own output would not list, browse or extract. And the child's stdin is `/dev/null`:
a password-protected archive makes `unzip`/`7z` prompt, and with the terminal still attached the child
waits for typed input while the UI sits on a progress bar that never moves. With `/dev/null` the prompt
gets EOF and the tool fails with an error the status line can show.

**A dialog is a layout, and a path is one unbreakable word.** The pack dialog filled its fields to
`-FLT_MIN` and let its hint text run to the window edge, so a long archive name or a deep directory
came out clipped: this ImGui wraps only `TextWrapped()`/`PushTextWrapPos()`, never `Text()`, and a path
contains no space for a wrap to break at. Every sentence in the dialog now goes through a wrapping
helper; a labelled field fills to the edge with its own label width subtracted, because `-FLT_MIN`
measures from the cursor, which the label has already pushed past the edge; and the two path lines are
shortened in the middle by `elide_middle()`, which binary-searches the widest split that fits and
keeps two characters of the head for one of the tail — the head says which project the path belongs
to, the tail names the leaf. `test_pack_dialog_layout` renders the dialog once per format and compares
`DC.CursorMaxPos.x` against the window's right edge.

**Long operations run between frames, never inside one.** A menu item, a context-menu entry or a
dialog button fires while its own ImGui frame is open, but copy, move, paste, extract, pack,
copy-in-place, links and directory sizes block and pump a progress UI of their own, which opens a
second frame inside the first and corrupts ImGui. They record an `AppDeferred` instead, and
`app_panes_run_pending()` carries it out between frames. That table is the only place a request becomes
an operation, and `test_deferred_ops` drives every constant in it and checks the result on disk: a
constant that falls through to the default case does nothing at all while the menu item that set it
still looks like it worked.

**The bindings were reserved before they were implemented.** They used to fall through to the plain
`F5`/`F6`/`F9` handlers, so `Alt+F5` quietly ran a copy and `Alt+F9` opened the pane menu.
`is_archive_key()` claims all five, and the plain `F5`, `F6` and `F9` handlers refuse `Alt`/`Ctrl`, so
an archive operation cannot regress into a copy again. `test_archive_keys_live` drives the real key
handler to keep that honest.

---

## 7. Order, effort, verification

| Order | Phase | Effort | Cumulative |
|---|---|---|---|
| 1 | 0 keymap migration | 1h | 1h |
| 2 | A marking | 2h | 3h |
| 3 | B clipboard | 3h | 6h |
| 4 | C quick filter | 3h | 9h |
| 5 | D navigation/panes | 3h | 12h |
| 6 | E views/dialogs (incl. compare, branch view) | 5h | 17h |
| 7 | F file ops | 3h | 20h |
| 8 | G trash | 1h | 21h |
| 9 | H multi-rename / quick search / columns | 2d | — |
| 10 | I keymap re-audit (Ctrl+P, Ctrl+I) | 1h | — |
| 11 | archives (backend, browsing, operations) | 2d | — |
| 12 | archive `..`, pack dialog, pane click focus | 2h | — |
| 13 | pack dialog layout, deferred operations | 2h | — |
| 14 | archive `F5` copy-out, folder extraction | 1h | — |
| 15 | operation dialog for `F5`/`F6`/`F7`/`F2`/`Shift+F4` | 2h | — |

Per phase: `cmake --build build` clean (LSP diagnostics on `src/*.cpp` must stay empty), then the
manual checklist below. Each phase ends with one commit whose message names the TC bindings it adds.

Manual checklist (extend per phase):
1. `+`/`-`/`*`/`/` marking behaves TC-style with an empty vs non-empty command line.
2. `Ctrl+C` → `Ctrl+X` → `Ctrl+V` across panes; cut clears the clipboard; abort keeps the remainder.
3. Filter active → arrow keys, PgUp/PgDn, `End`, mouse click and marks all address the *filtered*
   rows; changing directory clears the filter.
4. `Ctrl+U` swap keeps marks per side and restores the cursor by name (`sel_memory`).
5. Branch view on a deep tree stays under the entry cap and Esc returns to list mode.
6. Compare colours render under all 7 themes.
7. Command-line text editing still supports `Ctrl+C/X/V` when the command line is non-empty.
8. `Enter` on a `.zip`/`.tar.gz`/`.7z`/`.rar` browses it; `Tab`, `Ctrl+Tab`, `Ctrl+Left/Right`,
   `Ctrl+U`, the hotlist and `Alt+Left/Right` all carry the virtual path; `..` and `Backspace` leave one
   level and then the archive.
9. Inside an archive every mutation says no: `F2`, `F7`, `Shift+F4`, `F8`, `Ctrl+X`, `Ctrl+V`, `F6`,
   `Ctrl+B`, `Alt+F10`, `Alt+Enter`, `Alt+F7`, `F4`, `Alt+F3`.
10. `Alt+F5` → the dialog names the archive and picks a format → the file exists and `Alt+F9` on it
    gives the sources back; `Alt+Shift+F5` leaves only the archive behind; `Alt+F6` lands in the other
    pane; `Alt+Shift+F9` says "Archive OK".
11. `Ctrl+C` on two members, `Ctrl+V` in a real directory extracts those two and nothing else;
    `Ctrl+X` there refuses.
12. A pack or extract over a large archive still responds to Abort in the progress dialog, and the
    operation stops without leaving a partial status message claiming success.
13. In the pack dialog: a 7z with a password does not open without it, a tar with a password is refused
    in the dialog instead of writing an unencrypted archive, a zip with volumes is refused, and a 7z
    volume set lists and extracts as one archive.
14. Clicking below the last row, between rows, or on the scrollbar of the other pane makes that pane
    the active one; a password-protected archive reports a failure rather than freezing on a prompt.
15. Every Commands-menu and context-menu entry for a long operation — copy, move, paste, extract, pack,
    copy in place, link in other pane, directory sizes — runs from the menu with the progress dialog
    up and Abort working, instead of closing the menu and doing nothing.
16. The pack dialog at a narrow window width, with a long archive name and a deep target directory:
    no line runs past the frame, and an elided path keeps both its head and its leaf name.
17. Inside an archive, `F5` copies the cursor row, and with rows marked it copies those and only those;
    a marked folder comes out with its contents rather than as an empty directory, a second `F5` over
    the same target asks before overwriting, `F6` still refuses, and the archive itself is unchanged.
18. `F5` on one file: the `To:` field holds the whole target path and editing its last segment renames the
    copy. With two files marked it holds a folder, and typing a file name there is refused with a reason.
    A destination folder that is missing is refused without a trailing slash and created with one; `F5`
    onto the file itself is refused and the file is still there afterwards; `Esc` at any point leaves
    nothing behind; `F7` on a name already taken keeps the dialog open with the error in it, and the
    arrows and `Delete` move the cursor inside the field instead of walking the file list.
