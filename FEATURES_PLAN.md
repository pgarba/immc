# mc-clone — Total Commander parity plan

Spec source: `extern_dont_commit/tc_command.txt` (TC shortcut reference, © Thomas Bigler)
Audited against: `src/panes.cpp` (key handler `app_panes_handle_key` @1624-1865, special-key gate
`app_key_is_special` @1600-1622), `src/panes.h`, `src/filesystem.{h,cpp}`, `src/config.{h,cpp}`,
`src/theme.{h,cpp}`, `src/main.cpp` (event dispatch @104-160).

**Decisions this plan assumes**
1. **TC-faithful bindings.** Where our MC-style key collides with the TC table, TC wins and the MC
   binding is dropped or moved. Migration is done up-front (Phase 0) so later phases land on the
   final keymap once.
2. **Scope = parity minus archives.** Everything implementable without a new third-party dependency.
   Pack/unpack/browse (Alt+F5, Alt+Shift+F5, Alt+F6, Alt+F9, Alt+Shift+F9) is deferred, not dropped —
   see §6.
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

Config now also persists `view_mode`, `sort_mode`, `show_hidden`, `filter_preset`, `trash` and `columns`.

**Verification harness.** `tests/test_panes.cpp` includes `src/panes.cpp`, so the file-static helpers
(filter matcher, the `view` row map, marking, mark-undo, tabs, clipboard path helpers, branch view,
`fs_list_tree`, `fs_touch`/`fs_symlink`/`fs_trash`, quick search, the column spec, and the tree model)
run headless — 155 checks: `cmake --build build -j4 && (cd build && ctest)`. It caught two real bugs:
`?` was never handled in the glob matcher, and `tree_toggle()` kept a `vector` reference across an
`insert()`, so the `expanded` flag was written to freed memory and the next press duplicated the
row's children. Smoke run: `SDL_VIDEODRIVER=offscreen timeout 3 ./build/mc-clone`.

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
| F1 help | unbound | MISS | F1 → new `render_help()` key reference window |
| F2 refresh source dir | F2 = rename | see §5 | keep rename; refresh stays `Ctrl+R` |
| F3 view | F3 toggles floating quick view | PART | F3 → internal viewer (reuse `open_viewer`); quick view moves to `Ctrl+F1/F2`, `Ctrl+Q` |
| F4 edit | F4 terminal editor | OK | — |
| F5 copy / F6 move | ✓ + progress | OK | — |
| F7 mkdir | ✓ | OK | — |
| F8, Del delete | F8 ✓, Shift+Del ✓, plain Del no-op | PART | bind plain `Del`; add trash-vs-direct config (§4.7) |
| F9 activate menu | unbound | MISS | F9 → open menu bar / per-pane user menu |
| F10 activate menu | **F10 = quit** | CONFLICT | F10 → menu; quit becomes `Alt+F4` (+ keep `Alt+Q`) |
| Alt+F1 / Alt+F2 choose drive | unbound | MISS | volumes popup from `/proc/mounts` |
| Alt+F3 internal/external viewer | unbound | MISS | Alt+F3 → `xdg-open` on cursor entry |
| Alt+Shift+F3 plugin-less lister | — | OUT | no plugin system |
| Alt+F4 quit / minimize | Alt+Q only | PART | add `Alt+F4` |
| Alt+F5 / Alt+Shift+F5 pack | — | OUT | archives (§6) |
| Alt+F6 / Alt+F9 / Alt+Shift+F9 unpack, test | — | OUT | archives (§6) |
| Alt+F7 search | Shift+F7 | PART | bind `Alt+F7` (keep Shift+F7 as alias) |
| Alt+F8 command-history popup | Alt+PgUp/PgDn cycling only | MISS | history popup above the command line |
| Alt+F10 directory tree window | unbound | MISS | tree popup, Enter jumps |
| Alt+F11 / Alt+F12 focus dir bar | unbound | MISS | focus left/right path `InputText` |
| Alt+Shift+F11 buttonbar | no buttonbar | OUT | — |
| Alt+← / Alt+→ history | ✓ | OK | — |
| Alt+↓ visited-dirs list | unbound | MISS | history popup → jump |
| Shift+F1 custom columns view | unbound | MISS | low priority: columns/view popup |
| Shift+F2 compare directories | unbound | MISS | read-only pane compare |
| Shift+F3 show only cursor file when several marked | **Shift+F3 = cycle sort** | CONFLICT | sort → `Ctrl+F3..F7`; Shift+F3 = view cursor file only |
| Shift+F4 new text file + edit | unbound | MISS | create empty file, then F4 flow |
| Shift+F5 copy in same dir w/ rename | unbound | MISS | duplicate + rename prompt |
| Shift+Ctrl+F5 create links in target dir | unbound | MISS | `fs_symlink` |
| Shift+F6 rename in same dir | unbound | MISS | move-with-rename in place |
| Shift+F8 delete | Shift+Del only | PART | bind Shift+F8 |
| Shift+F10 local menu | right-click only | MISS | Shift+F10 + `SDLK_MENU` open `ctxmenu` |
| Shift+Esc minimize to tray | unbound | MISS | `SDL_MinimizeWindow` |

### Marking
| TC | Ours | Status | Action |
|---|---|---|---|
| Num+ extend / Num- shrink | `+`/`-` = select-all / clear (when cmd empty) | CONFLICT | TC semantics: mark/unmark at cursor and step; select-all/clear move to `Ctrl+Num+`/`Ctrl+Num-` |
| Num* invert | `*` ✓ | OK | — |
| Num/ restore marks | unbound | MISS | snapshot stack of `marked` before each bulk change |
| Ctrl+Num+ all / Ctrl+Num- none | Ctrl+A / Ctrl+U | PART | add Num forms; `Ctrl+U` freed for pane swap |
| Ctrl+Shift+Num+ files+dirs, Ctrl+Shift+Num- files-only, Shift+Num+/-/* dir nuances | unbound | MISS | add `mark_scope(files\|dirs\|both)` to the mark helpers |
| Alt+Num+ / Alt+Num- mark/unmark same extension | unbound | MISS | ext match on cursor entry |
| Ins mark | ✓ (marks + advances) | OK | — |
| Space mark (+ dir size sum) | Ctrl+Space only | CONFLICT | Space marks when cmd line empty, else types a space; Space on unmarked dir fills computed size into the size column |
| Ctrl+A select all | ✓ | OK | — |
| Shift+arrows / Shift+PgUp/PgDn range mark | ✓ | OK | — |

### Navigation, tabs, panes
| TC | Ours | Status | Action |
|---|---|---|---|
| ↤ / Ctrl+PgUp parent dir | Backspace ✓, **Ctrl+PgUp = prev tab** | CONFLICT | Ctrl+PgUp → parent; also bind `\`; tabs move to `Ctrl+Tab` |
| Ctrl+PgDn open dir under cursor | **Ctrl+PgDn = next tab** | CONFLICT | Ctrl+PgDn → open under cursor in this pane |
| Ctrl+← / Ctrl+→ open under cursor in other pane | unbound | MISS | cursor stays put |
| Ctrl+< root | unbound | MISS | `Ctrl+Shift+Comma` → `/` |
| Ctrl+I switch to target pane | unbound | MISS | alias of Tab |
| Ctrl+R refresh | ✓ | OK | — |
| Ctrl+T new tab (active) | ✓ | OK | — |
| Ctrl+Shift+T new tab (inactive) | unbound | MISS | `tab_new(pane, activate=false)` |
| Ctrl+W close tab | ✓ | OK | — |
| Ctrl+Shift+W close all tabs | unbound | MISS | keep ≥1 tab |
| Ctrl+↹ / Ctrl+Shift+↹ next/prev tab | Ctrl+PgUp/PgDn | CONFLICT | move to Ctrl+Tab; keep Ctrl+1..9 |
| Ctrl+U swap directories | **Ctrl+U = clear marks** | CONFLICT | Ctrl+U swaps pane dirs; clear marks → Ctrl+Num- |
| Ctrl+Shift+U swap dirs **and** tabs | unbound | MISS | swap whole tab sets |
| Ctrl+↑ open cursor in new tab (same pane) | unbound | MISS | |
| Ctrl+Shift+↓ open cursor in new tab (other pane) | unbound | MISS | |
| Tab switch panes | ✓ | OK | — |
| Backspace → parent | ✓ | extra | keep (not in TC table, additive) |
| Home → first entry / Shift+Home → $HOME | ✓ | extra | keep |

### Views / dialogs
| TC | Ours | Status | Action |
|---|---|---|---|
| Ctrl+F1 quick view short / Ctrl+F2 quick view long | F3 floating quick view | PART | quick view bound to Ctrl+F1 (name+size) / Ctrl+F2 (full columns) |
| Ctrl+Q quick-view **pane** instead of a file pane | floating window only | PART | replace the inactive pane's list with a quick view |
| Ctrl+F3..F6 sort name/ext/date/size, Ctrl+F7 unsorted | cycle Shift+F3 | MISS | direct sort keys; add `FS_SORT_NONE` |
| Ctrl+F8 / Ctrl+Shift+F8 tree | unbound | MISS | toggle the Alt+F10 tree (embedded later) |
| Ctrl+B branch view / Ctrl+Shift+B marked branch view | unbound | MISS | flat recursive listing in-pane |
| Ctrl+S quick filter / Ctrl+Shift+S re-apply / Ctrl+F10 clear | unbound | MISS | per-pane live filter |
| Ctrl+F11 executables-only / Ctrl+F12 user filter | unbound | MISS | filter presets; `filter_presets` in config |
| Ctrl+D directory menu (hotlist) | Alt+D add, Shift+F9 list | PART | Ctrl+D opens the hotlist popup |
| Ctrl+L size of marked | unbound | MISS | reuse `fs_count_tree` |
| Alt+Shift+Enter sum all dir sizes | unbound | MISS | fill size column for dirs |
| Alt+Enter properties | ✓ | OK | — |
| Enter open/run/command | ✓ (cmd-first) | OK | — |
| AltGr+letter / Ctrl+Alt+letter quick search by name | unbound | MISS | type-ahead jump in the pane |
| Shift+Enter `cmd /c` NOCLOSE | — | OUT | Windows-specific |

### Clipboard & rename
| TC | Ours | Status | Action |
|---|---|---|---|
| Ctrl+C copy to clipboard / Ctrl+X cut / Ctrl+V paste | unbound (no-op) | MISS | app-level file clipboard (§4.4) |
| Ctrl+M multi-rename tool | unbound | dropped | user decision: not needed |
| Ctrl+P current path into command line | Alt+C prefills `cd ` | MISS | `Ctrl+P` → `cmd = active.current_dir` |
| Ctrl+Z comments | — | OUT | no comment store |

### Out of scope (Windows/legacy or dependency-bound)
FTP (`Ctrl+F`, `Ctrl+Shift+F`, `Ctrl+N`, `Ctrl+Shift+M`) · thumbnails (`Ctrl+Shift+F1`) · comments
(`Ctrl+Z`, `Ctrl+Shift+F2`) · printing (`Ctrl+F9`) · NOCLOSE.PIF · buttonbar (`Alt+Shift+F11`) ·
drive letters (`Alt+F1/F2` → implemented as mount picker instead) · archives (§6).

**Count:** ~30 exact matches, ~14 partial, ~35 missing, 6 conflicts to migrate, ~14 out of scope.

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
- `Shift+F5`: copy cursor entry to `name.copy` (or prompt via `OP_RENAME`-style op bar) in the same
  dir; `Shift+F6`: same-dir move/rename (rename prompt, `fs_move_entry`).
- `Shift+Ctrl+F5`: `fs_symlink(src_abs, dst)` in the other pane.
- `Ctrl+L`: `fs_count_tree` over marked → status line.
- `Alt+Shift+Enter`: fill the size column for all dirs (bounded, cancellable with Esc).
- fs additions in `filesystem.{h,cpp}`: `fs_symlink`, `fs_touch`, `fs_list_tree`, `fs_trash`.

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
| `Backspace` unbound | `Backspace` = parent dir | additive convenience; TC's `\` and `Ctrl+PgUp` also bound. |
| `Ctrl+O`, `Alt+.`, `Ctrl+1..9`, `Alt+C`, `Alt+PgUp/PgDn` | kept | MC extras not in the TC table; no collisions after Phase 0. |
| `Ctrl+B` branch view is a full browser | branch view is capped (depth 6, 4000 rows) and `F2`/`F7` are refused inside it | the subtree is a snapshot; rename/mkdir address rows by name only, so they are blocked instead of silently hitting the wrong file. Delete/copy/move/view go through `entry_rel()` and are correct. |
| `Shift+F2` compares content | compares name + size + type | content compare needs a diff engine; size/type mismatch is the 95% case. Colors come from the new theme slots `diff_missing` / `diff_differ`. |
| `AltGr+letter` quick search | same, detected as `SDL_KMOD_MODE` or `SDL_KMOD_RALT` | SDL3 has no `SDL_KMOD_ALTGR`. The binding is tested before the `Alt+letter` handlers so `AltGr+Q` searches instead of quitting. |
| tree panel keyboard | embed mode: keyboard drives the tree; split mode: keyboard stays with the file list | in a split panel there is no third focus to cycle to, and `Tab` already switches panes in TC. The tree is mouse-driven in split mode. |

---

## 6. Deferred: archives

`Alt+F5` pack · `Alt+Shift+F5` pack+delete · `Alt+F6`/`Alt+F9` unpack under cursor ·
`Alt+Shift+F9` integrity test · browsing an archive as a pane. Needs a dependency (zip-only via
`miniz` vendored in `extern_dont_commit`, or `libarchive` for zip+tar). Design when picked: an
`ArchiveBackend` interface behind `list_directory`/`fs_copy_entry` with a pane "inside archive" flag;
branch/compare/filter then need to skip archive modes. Estimated 2-4 days — the single largest item.

**The bindings are reserved anyway.** They used to fall through to the plain `F5`/`F6`/`F9` handlers,
so `Alt+F5` quietly ran a copy and `Alt+F9` opened the pane menu. `is_archive_key()` now claims all
five and the status line says which archive operation is missing; the Commands menu shows
"Pack archive" / "Extract archive" greyed out so the shortcuts are discoverable. The plain `F5`, `F6`
and `F9` handlers also refuse `Alt`/`Ctrl`, so a future archive implementation cannot regress into a
copy again.

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
| — | archives | 2-4d | — |

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
