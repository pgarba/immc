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
| J app identity & shell | done | `src/version.h.in` → generated `version.h`; `--version`/`--help` before SDL starts; title bar `immc - <active dir>` with middle elision; menu bar `File`/`Commands`/`Options`/`Help`; Options dialog with a theme picker and a font picker over the system and user font directories; theme, font and size persist |
| K text rendering | done | the glyph atlas goes through FreeType when `freetype2` is installed (`find_package(Freetype)` → `MC_HAVE_FREETYPE`, `imgui_freetype.cpp` linked in), and stays on stb_truetype when it is not; a named hinting mode (`default`/`none`/`light`/`mono`/`mono (no smoothing)`) in the Options dialog and as `font_hinting` in the config; the window is created high-pixel-density and the atlas is rebuilt when the display scale changes; the size rides on `style.FontScaleMain` for every font, because the size passed to `AddFont()` is read once, on the first frame, and never again |
| L remote sites | done except L3 | `src/remote.{h,cpp}` — libcurl transport, `0600` sites file, MLSD and LIST parsers — and the remote pane mode in `panes.cpp`, compiling **and linking** with and without libcurl. `Ctrl+F` manager, `Ctrl+N` quick connect, `cd ftp://…`, and upload/download/mkdir/rename/delete/chmod are wired. `test-remote` runs 295 checks with 0 failures (188 without libcurl), `ctest` is 4/4 in both configurations, and the offscreen smoke run stays up in both. A remote copy runs on a worker thread with a transfer list under the panes, and quick view previews a row on a site or inside an archive — both are in §8.1, along with what libcurl forced into the design and what is still open. |

Config now also persists `view_mode`, `sort_mode`, `show_hidden`, `filter_preset`, `trash`, `columns`,
`font_hinting` and the size of the F3 quick view window (`quickview_w`/`quickview_h`). The hinting value
is a name and not a number, so a hand-edited config says what the dialog shows and a name this build does
not recognise is dropped back to `default` rather than guessed at. The quick view's size lives in the
config because `io.IniFilename` is null — ImGui keeps a floating window's shape for a session and writes
nothing down — and its position is deliberately not carried: coordinates from a monitor that has gone
leave a window whose title bar cannot be reached.

**Archives (§6) closed the last missing binding**, and inside an archive `F5` copies the marked
members out into the other pane, as TC's does. Four rows stay partial, each a deliberate shape
difference rather than an unimplemented shortcut: `F3` toggles the quick view instead of opening the
internal viewer (`Enter` and Shift+F3 do that), `Ctrl+F1`/`Ctrl+F2` set the view mode instead of a
short/long quick view, `Ctrl+D` is unbound while the hotlist is on `Shift+F9`, and the mark helpers
can scope to files but the dirs-only variant is not reachable from the keypad.

**Verification harness.** `tests/test_panes.cpp` includes `src/panes.cpp`, so the file-static helpers
(filter matcher, the `view` row map, marking, mark-undo, tabs, clipboard path helpers, branch view,
`fs_list_tree`, `fs_touch`/`fs_symlink`/`fs_trash`, quick search, the column spec, the tree model, the
archive pane mode with its guards and operations including copy-out, the deferred-action table, the
operation dialog with its destination rules, the font scan with its extension, depth, cap and
no-duplicates rules, the font fallback when a named file is not a font, the hinting table with its
name round-trip and its translation to the loader's flags, the font size reaching the height a frame
actually draws at for either font, the framebuffer scale derived from a
window's logical and pixel sizes, the identity strings, the title
bar's elision, the Options dialog's keys and layout, the menu bar actually being
drawn, and the quick filter taking the caret with the keys typed straight after
it) run headless — 1692 checks in `test-panes` plus 220 in `test-archive`:
`cmake --build build -j4 && (cd build && ctest)`.
Focus is a place where "it compiles" and "it works" part company. ImGui keeps one
keyboard-focus request per frame and the last one submitted wins, and the command
line at the bottom of the window asks on every frame - with offset `-1`, which
resolved to no focusable item at all, so the command line was never the active
item and every field that had asked first was ignored. `claim_focus()` marks the
claim so the command line stands down, it asks for itself with offset 0, and it
steps back while the filter, a path bar or the Options field holds the keyboard.
Asking for the keyboard is also how a click gets lost: the request is answered on
the *next* frame, so it lands in the middle of a press and takes `ActiveId` off
whatever is being clicked - the release then finds a different item and the click
is dropped. That is what made the menu bar titles drop their menus and the items
in them do nothing. The command line asks only while the keyboard is free: nothing
active, no popup open, nothing claimed. The other half is that a request is answered
against whichever window holds focus a frame later, so a window appearing in between
- the F3 preview, a dialog - swallows it, and the filter's request is repeated until
the field really has the keyboard. Both are tested with a virtual mouse: press and
release on a menu title, then on its first item, and check the item fired.
The identity is checked from the terminal too: `ctest` runs `mc-clone --version` and matches the name
and the version in its output, the one path that has to work with no display at all.
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
Text rendering is the one area where a headless check can only go so far, so it is checked as far as
it can: the test asserts the atlas really answers to the FreeType loader (`io.Fonts->FontLoader ==
ImGuiFreeType::GetFontLoader()`) rather than merely that the library was linked, that the mode name
reaches the loader as flags, and that the flags change the raster — the same face at the same size
lands on different pixels under `none` than under `mono`, which is what separates a working setting
from one that is stored and ignored. Whether a 2x monitor is honoured is not reproducible here (no
Xvfb on this machine), so the arithmetic is tested instead: `mc_framebuffer_scale()` returns the ratio
the SDL3 backend writes into `io.DisplayFramebufferScale.x`, and falls back to 1x on a minimized or
half-measured window.
Size is checked the same way, and it had a real bug behind it. ImGui since 1.92 bakes glyphs at the
size the *style* asks for; the size handed to `AddFont()` only becomes that font's default, which
ImGui reads once, on the first frame, to seed its base size. A slider wired to it therefore set the
size at startup and did nothing after: the atlas was rebuilt, and the text stayed the same height. The
test measures `ImGui::GetFontSize()` inside a real frame — the height the UI draws at — for the
built-in font and for a TrueType file resized after that first frame, and then checks the raster
followed: a baked `H` at 30 px is most of 30 px tall, not of 13.

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
| Windows | `render_quick_view` @1055 (floating child), `render_viewer` @1096, `render_output` @1119, `render_hotlist` @1131, `render_search` @1164, `render_confirm` @1216, `render_about` @1234, `render_properties` @1272, `render_options` (theme, size, font list with a filter), `render_progress_dialog` @530 | quick view is a *floating window*, not a pane replacement. |
| Menu bar | `app_panes_render` | `File` (the file operations and the internal clipboard, TC's Files menu), `Commands` (the rest), `Options` (theme, font, hidden, columns, sort, trash), `Help` (About). `F10` opens the `Commands` items as a keyboard menu from the same `render_commands_items`, so the two cannot drift. The root window has to carry `ImGuiWindowFlags_MenuBar`: without it `BeginMenuBar()` returns false and the menus exist in the source and nowhere on screen. |
| File list render | `render_file_list` @850-1053 | table for MID/FULL, `ImGui::Columns` for BRIEF; row backgrounds drawn manually via `GetWindowDrawList`. Right-click popup `ctxmenu` @1007-1048. |
| Config | `config.h` | persists window geom, font, theme, tabs, `view_mode`, `sort_mode`, `show_hidden`, `filter_preset`, `trash`, `columns`, `font_hinting`, `quickview_w`/`quickview_h`. The Options dialog writes the same three fields the startup reads, so what the session showed is what the next one starts with. |
| Text rendering | `panes.cpp` `kHintingModes`/`mc_freetype_loader_flags`/`app_font_apply`, `mc_framebuffer_scale`; `main.cpp` window creation and `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` | The hinting table is one array of `{name, bits, note}`: the name is what the config stores and what the combo shows, the note is the line the dialog says about the cost of picking it. `mc_freetype_loader_flags()` is the only place the app's names meet FreeType's flags, and it compiles in a build without FreeType, where there is a vocabulary to remember but nothing to honour it. The window is created with `SDL_WINDOW_HIGH_PIXEL_DENSITY`; without it the compositor stretches a 1x framebuffer and every glyph is a bitmap blown up to twice its size. |
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
| F10 activate menu | `F10` opens the Commands menu | DONE | the menu bar carries `File`/`Commands`/`Options`/`Help`; quit is `Alt+F4` (+ `Alt+Q`) |
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
FTP (`Ctrl+F`, `Ctrl+Shift+F`, `Ctrl+N`, `Ctrl+Shift+M`) — no longer out of scope, planned as
Phase L in §8 · thumbnails (`Ctrl+Shift+F1`) · comments
(`Ctrl+Z`, `Ctrl+Shift+F2`) · printing (`Ctrl+F9`) · NOCLOSE.PIF · buttonbar (`Alt+Shift+F11`) ·
drive letters (`Alt+F1/F2` → implemented as mount picker instead) · writing into RAR (no writer in
`7z`, so RAR is read-only) · archive *mutations* (rename/delete/move inside an archive).

**Count:** 74 bindings in the table — 58 bound (16 already matched at audit time, 42 implemented by
the phases below), 4 partial, 8 conflicts resolved by the Phase 0 migration, and the 4 FTP bindings
planned as Phase L (§8) instead of out of scope.

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

**`Tab` walks the dialog, and `Enter` runs what it lands on.** The ring is the dialog read top to bottom:
the field, the verb (`Copy`, `Move`, `Rename`), `Run in background` when the plan crosses a network, and
`Cancel`; it wraps back through the field, because that is where the destination gets corrected. `Enter`
then means the same thing it means on the field wherever it lands — commit, commit-with-background, or
close — so the dialog is finishable without reaching for the mouse, which until now it was not: a copy to
a site could be started from the keyboard and had to be aimed at `Run in background` with a pointer.
`Space` is deliberately not an activation key, because the field keeps the caret while the ring is on a
button and a space is a character in a path. ImGui's own navigation is off in this app — the panes own the
arrow keys, and nav would take them — so the ring is the app's state and its mark is drawn by hand with
`cursor_outline`, the same outline the pane puts around its cursor row. Two things this sits on: the ring
length is asked of the plan through one helper, so the drawing and the key handler cannot disagree about
whether `Run in background` is on screen; and `Tab` is intercepted before the pane handler, because it is
the pane switch when no dialog is up, and moving a pane underneath an open copy dialog would change what
the operation acts on.

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
| 16 | app identity, title bar, menu bar, Options dialog | 2h | — |
| 17 | FreeType loader, hinting mode, high-DPI window | 2h | — |
| 18 | L1 remote sites: libcurl transport, sites file, site manager, browse, download | 1d | — |
| 19 | L2 remote writes: upload, mkdir, rename, delete, chmod, properties | 0.5d | — |
| 20 | L3 remote work off the frame loop with `curl_multi` | 0.5d | — |
| 21 | M pane status bar (sizes, file/dir counts, marked subset) | 1h | — |

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
19. The title bar reads `immc - <the active pane's directory>` and follows `Tab`, `Ctrl+Tab`, `Alt+Left/Right`
    and entering an archive; a path too long for it loses its middle and keeps its last segment.
    `File`, `Commands`, `Options` and `Help` all open at a narrow window width and every entry does what
    its shortcut does — `Rename`, `Copy`, `Move / rename`, `Delete`, `Paste`, `Quit` from `File`; theme and
    sort from `Options`. `Options > Fonts and theme...` opens, with `Esc` closing it: the theme combo
    repaints immediately, the size slider re-scales the text — for the built-in font and for a TrueType
    font alike — and picking a font from the list
    re-lays-out the whole UI between frames. Typing in the filter box narrows the list and does not move
    the pane's cursor; a `.ttf` that is not a font leaves the built-in font on screen with the reason in
    the dialog, and the settings do not keep the bad path. `mc-clone --version` prints the name, the
    version and the config path in a terminal with no display.
20. The four menu titles are on screen at the top of the window, not only in the source, and the panes
    start below them. **Clicking a title drops its menu, and clicking an item in it does the thing the
    item says** - the same is true of the right-click menus and the combo box in the Options dialog, so
    try one of each. The panes start below the bar. `Ctrl+S` opens the quick filter with the caret already in the field, so the next
    keys typed narrow the list instead of landing in the command line; `Ctrl+S` in the other pane focuses
    that pane's field. With the filter holding the keyboard, `Esc` leaves it and the command line takes
    the typing back.
21. `mc-clone --version` says which rasterizer the build has. In `Options > Fonts and theme...` the
    hinting combo offers the five modes and one line under it says what picking one costs; switching to
    `mono` at 13-15 px makes the stems land on whole pixels, `none` makes them grey, and `light` keeps
    the spacing between glyphs. The choice survives a restart, and so does a hand-edited
    `font_hinting = mono` in the config; a name that is not one of the five comes back as `default`
    rather than being reinterpreted. On a build without `freetype2` the combo is gone and the dialog
    says the text is rasterized with stb_truetype, which does no hinting. Drag the window to a monitor
    with a different scaling factor, or change the desktop scaling: the text re-sharpens without a
    restart, and `Help > About` shows the window scale it settled at. The size slider belongs to both
    fonts: pick a TrueType file, move it, and the file list re-lays-out at the new height — 0 px puts the
    built-in 13 px back, and the size in the dialog's status line is the height the text is drawn at.

---

## 8. Phase L — remote sites: FTP, FTPS, SFTP

`Ctrl+F` opens a saved-site manager, `Ctrl+N` a quick-connect line, and a pane can then browse a
server the way it browses a directory. This section records the decisions and the shape, so the
implementation session starts with the arguments settled rather than re-litigating them against 5,500
lines of `panes.cpp`. It is now a record of a built feature: §8.1 says what exists, what libcurl forced
into the design, and what is still open.

| Decision | Choice | Why |
|---|---|---|
| Transport | link `libcurl`, optional at build time | one login and one TLS session stay open across pane navigations, and the credentials reach the handle in memory instead of argv, where `/proc` would show them. `find_package(CURL QUIET)` → `MC_HAVE_CURL` is the FreeType pattern: without the dev package the app still builds, and the dialog says this binary has no FTP support instead of offering a connect that cannot work. |
| Saved passwords | `~/.config/mc-clone/ftp_sites`, created `0600` | the main config is written with `fopen(..., "w")` and lands `0644`, world-readable, so it must stay password-free. Cleartext in a private file, per site, with a "remember password" tick — and the dialog says in one line that it is cleartext. |
| Quick connect | a `Ctrl+N` field **and** `cd ftp://…` on the command line | `exec_command()` already parses `cd` with `~` expansion and probes with `opendir()`; a scheme check in front of that probe is the whole change. The field connects without saving anything. |
| Write scope | full read/write | browse, download, upload, `MKD`, `RNFR`/`RNTO`, `DELE`/`RMD`, `SITE CHMOD`. Delete is direct: a server has no trash, the `trash` setting is not honoured here, and the confirm prompt says so rather than implying a recovery path. |

**A remote pane is a pane mode, not a parallel UI.** The same trick archives use: `current_dir` may be
a virtual path — `ftp://user@host:2121/pub/incoming` — and `remote_split_virtual()` splits it into the
site part and the path on the server. Tabs, tab labels, directory history, the path bar, the hotlist,
filters, marks and `Ctrl+Left/Right` then carry a remote location without knowing FTP exists. A
`Pane::remote_site` + `remote_path` pair and an `in_remote(pane)` guard mirror
`archive_path`/`archive_prefix`/`in_archive()`, which 18 call sites consult today — that count is the
honest price of this design, and it is paid once. A site root lists a `..` row like an archive root,
so Enter leaves the site. The password is never part of a displayed path, and `Ctrl+P` puts the URL on
the command line without it.

**Listings fill `FileEntry` and stop there.** `MLSD` first — machine-readable, and it carries `type`,
`size`, `modify` and `unix.mode` — falling back to parsing `LIST` when the server does not implement
it, in both the unix and the DOS dialect and with names containing spaces. Both produce
`FileEntry{name, mode, size, mtime}`, so `compare_files()` and the column formatting are untouched.
A single row's metadata comes from listing its parent — `MLST` looked reachable through
`CURLOPT_CUSTOMREQUEST` and is not, see §8.1.

**TLS is a per-site mode, and the mode stays visible.** `ftp://` with `CURLUSESSL_TRY` upgrades with
`AUTH TLS` when the server offers it; explicit FTPS is `CURLUSESSL_ALL` with `AUTH=TLS`, and — contrary
to how this paragraph first read — it is still sent to curl over an `ftp://` URL, because libcurl reads
an `ftps://` *scheme* as implicit TLS; see §8.1. Implicit FTPS is `CURLFTPAUTH_SSL` on port 990.
Verification is on by default (`VERIFYPEER`, `VERIFYHOST=2`). A self-signed server can be connected
anyway, per site, and while that flag is set the site list, the edit dialog and the pane's status line all
read `certificate not verified` — a security mode that degrades silently is the one thing not to get
wrong here. `sftp://`/`ssh://` rides along with libcurl's libssh2 backend almost for free; the host key
is trust-on-first-use with the fingerprint shown and remembered in the sites file, which is the weakest
part of the feature and worth saying out loud.

**Operations reuse the copy pipeline.** Download and upload go through the existing progress dialog:
`CURLOPT_XFERINFOFUNCTION` has the same shape as `fs_progress_fn` — returning true aborts there,
non-zero aborts in curl — so the dialog's Abort stops a transfer the way killing a child stops `unzip`.
`F5` between a remote pane and a local one is the direction test. `F6` between two remote panes is a
separate question — a server-side rename when both sit on one host, otherwise download, upload, delete
— and should refuse until that is answered. Refused inside a remote pane, as inside an archive: branch
view, tree panel, directory sizes, search, the editor, "open with". Quick view is the exception — it
previews a row on a server and a member of an archive, and §8.1 records what that took. An archive *on* a
server is two steps — fetch it to a temporary file, then browse it, the `arch_extract_temp()` analogue
— and is a stretch goal, not part of L1.

**Blocking is the open risk, and it has a named answer.** `src/` starts no threads today; long
operations run synchronously and pump a minimal progress frame, which is what L1 does, with
`CONNECTTIMEOUT_MS` and `LOW_SPEED_LIMIT`/`LOW_SPEED_TIME` so a dead server ends instead of hanging,
and Abort through the xferinfo callback. A LAN listing is one round trip and feels instant; a WAN
listing leaves the window unresponsive for 200-500 ms. If that is not acceptable, the fix is
`curl_multi_*` driven once per frame — no threads, the pane fills in when the transfer completes —
which is phase L3 and a larger change than L1, so it is a phase and not a footnote.

The answer the build took is the one this paragraph argued against. Transfers and previews run on worker
threads, one connection per thread, and `curl_multi_*` is left to listings, which still block.

**Testing is a localhost server, not a real one.** `tests/ftp_server.py`, about 100 lines: an FTP
server on `127.0.0.1` with an ephemeral port serving a fixture tree, speaking
`USER/PASS/PWD/CWD/PASV/LIST/MLSD/RETR/STOR/MKD/DELE/RNFR/RNTO/TYPE/SIZE/QUIT`, plus an `AUTH TLS`
variant whose certificate the test generates with `openssl req -x509` (Python 3.14 is present,
`pyftpdlib` is not, so the server is written here). Over it: connect, list, download and compare
bytes, upload, mkdir, rename, delete; and a transfer the server stalls past `LOW_SPEED_TIME`, which
must report failure rather than success — the same honesty rule the archive code has. Unit-level: URL
parsing (scheme, port, IPv6, `%xx`, and no password in the display form), the sites-file round-trip
**including its `0600` mode**, MLSD and both LIST dialects and the pane rows they produce, and which
scheme sets which curl options. The TLS test asserts that verification-on refuses a self-signed server
with a readable message, that the per-site override connects, and that `certificate not verified`
reaches the UI. A build without libcurl must compile and answer "this build has no FTP support", the
same shape as the no-FreeType build. `mc-clone --version` prints `FTP: libcurl 8.22.0 (OpenSSL 3.6.5)`
or `FTP: not in this build`, and `test_cli.cpp` grows the assertion.

**Keys.** `Ctrl+F` (site manager) and `Ctrl+N` (quick connect) are unbound today, and both are already
reserved in §2's out-of-scope line. `Ctrl+Shift+F` and `Ctrl+Shift+M` are the other two FTP bindings
that line records; `tc_command.txt` is not in the repository, so those two stay unbound until the spec
line can be read. A connect that fails leaves the site dialog open with curl's message in it, the way
the pack dialog refuses with a reason instead of closing.

**Effort.** L1 transport + sites file + site manager + browse + download ~1d. L2 writes (upload,
mkdir, rename, delete, chmod, properties) ~0.5d. L3 `curl_multi` so listings and transfers do not
freeze the frame loop ~0.5d. SFTP host-key UX ~2h, deferrable.

### 8.1 Where the build stands

Written, and compiling both with and without libcurl: `src/remote.{h,cpp}` — protocol vocabulary, URL
parsing, the `0600` sites file, the MLSD and LIST parsers, the cached-handle transport, and the tree
helpers `net_download_tree`/`net_upload_tree`/`net_remove_tree` — plus the remote pane mode in
`panes.cpp`: `Pane::remote_root`/`remote_path`/`remote_came_from`, `LIST_REMOTE`, `in_remote()`, and a
`refresh_pane()` that splits `ftp://…` before it tries an archive path. The `Ctrl+F` manager stages its
fields in a `NetForm` so Cancel means nothing happened; `Ctrl+N` takes one line; both are in the
Commands menu. `cd ftp://…` and a bare URL go through `remote_open_url()`, which reuses a saved site's
credentials for the same root. Guards refuse branch view, tree mode, search, directory sizes, the
editor, the clipboard, extract-into, and "open with" inside a site; delete says the server has no
trash; viewing downloads to a temp directory the viewer already cleans up. `--version` and the About
window print `net_what()`.

`tests/ftp_server.py` is the fixture: passive mode only, one user, jailed to `--root`, one JSON line
naming the port it bound, `AUTH TLS` when given a certificate, and `FTP_SERVER_LOG=1` to watch the
protocol on stderr — without that, a client bug looks exactly like a server bug. `test-remote` splits
the pure half (URLs, virtual paths, the sites file and its mode, both listing dialects, `unix.mode`)
from the localhost half, and skips the second when the build has no libcurl or the fixture will not
start.

**Five things libcurl does that the design above assumed otherwise.** All of them were found against
the fixture, and all are settled in the code now.

- A listing URL has to end in `/`. Without the slash curl sends `SIZE <last component>` to decide
  file-or-directory, a directory answers `550`, and curl reports *The file does not exist* without ever
  sending `MLSD`. `dir_url()` appends it — and `quote_cmds()` needs it for the same reason, because its
  URL is a directory too: with `FAILONERROR` every mutation in a directory deeper than the root failed.
  That is the shape of the original failure list: uploads passed and mutations did not, because
  `net_upload()` names a file and was never affected.
- `CURLOPT_CUSTOMREQUEST "MLST"` on a file URL is ignored: curl still sends `SIZE` then `RETR` and the
  fact line never arrives. `net_stat()` now lists the parent and matches the basename, for every
  protocol, which replaced two code paths with one. It also makes *one MLST per entry* in
  `plan_existing()` wrong twice over — the new `net_stat()` would pay a parent listing per name — so
  that does one `net_list()` of the destination directory and looks the names up in it.
- `ftps://` is **implicit** TLS to libcurl: it starts the handshake before it has sent a single FTP
  command, which against a server waiting for cleartext is `wrong version number`. Explicit FTPS has to
  travel as `ftp://` with `CURLOPT_USE_SSL` set to `ALL`. The site's root string keeps the `ftps://` the
  user typed, because that is what the path bar shows and it is the key that tells two saved sites
  apart; `curl_url()` swaps the scheme at the transport boundary, and only `NET_FTPS_IMPLICIT` keeps
  `ftps://`.
- `CURLOPT_FTP_CREATE_MISSING_DIRS` makes curl answer a failed `CWD` with `MKD`, so *listing a path that
  does not exist creates it on the server*. That belongs on a store and nowhere else, so it is set in
  `net_upload()` alone.
- `CURLOPT_QUOTE` runs after curl's own `CWD`, and curl decides whether it needs one from the directory
  it remembers on the *handle* — while the connection it reuses sits wherever the last transfer left it.
  `RNFR put.txt` was answered `550` with the session sitting in `/partial`. `quote_cmds()` now puts an
  absolute `CWD` at the head of the quote list, which runs after anything curl does.

**The server root is spelled two ways and callers have to know both.** `net_path_parent()` returns `""`
for the root while `net_split_virtual()` returns `"/"` for a typed `ftp://host/`, so `net_path_is_root()`
is the question and `activate_entry()` asks it. The old condition tested `remote_path[0]`, which made a
pane at the root navigate to the root forever instead of leaving the site. `net_join_remote()` is the
matching join: the naive `dir + "/" + name` produced `//name` out of the root, which a server that
honours the empty component reads as a directory with no name in it.

**The fixture was wrong in two ways that hid client bugs.** It closed its TLS data connection without a
`close_notify`, so curl reported `SSL_read: unexpected eof while reading` after the listing bytes had
already arrived; `close_data()`/`close_ctl()` now `unwrap()` first, guarded on
`isinstance(…, ssl.SSLSocket)` — a plain socket has no `unwrap()`, and the resulting `AttributeError`
took the single-process listener down and turned one bug into 39 failures, which is what a fixture that
dies mid-session looks like. And its MLSD rows left out the space RFC 3659 puts between the facts and
the entry, so a parser that took the whole tail after the last `;` passed every test while giving every
name on a real server a leading space — the thing a user notices as a space appearing in front of a
directory or file name the moment a row is opened or fetched. The fixture emits the conformant row now,
and the parser strips exactly one separator space, so a name that genuinely starts with a space (sent as
two) keeps one.

**A build without libcurl did not link.** `MC_REMOTE_SOURCES` was empty in that configuration, so
`remote.cpp` was never compiled and `panes.cpp` ended up with 114 undefined references to `net_*`. The
module is compiled either way now; only the library and `MC_HAVE_CURL` are conditional. The claim that
this configuration built had never actually been checked — it is checked in both `ctest` runs now.

**A preview of a file that is not on this disk fetches it on a thread.** `F3` and `Ctrl+Q` read the
beginning of the row under the cursor, and both used to say `unreadable` about a text file the pane was
looking straight at, because the only reader was `fopen()` and neither a site's path nor an archive
member's is a path `fopen()` knows. `net_download_prefix()` takes the first 64 KB — FTP cannot be asked
for a range that ends early, `REST` sets a starting point and the server sends everything after it, so
the transfer is abandoned once the bytes arrive, which is what makes a preview of a 4 GB log cost the
same as one of a 4 KB file — and `arch_extract_temp()` unpacks a member into a temporary directory the
fetch removes. A local file is still read inside the frame, because it costs microseconds; anything else
goes on a worker, because a WAN fetch is slower than the frame that asked for it. The answer lands in a
later frame, filed under the path it was *asked for*: the cursor moves with the arrow keys, so an answer
is almost always about a row the user has already left. `qv_reap()` at the head of each frame joins the
fetches that have finished and tells the ones no quick view asked for in the frame that just drew to stop
pulling bytes. That question is asked there, and not where a new fetch starts, because two quick views
can be open at once — `F3` over each pane, or `F3` and the `Ctrl+Q` panel — and a rule that cancelled
everything else in order to start one view's fetch cancelled the other view's, which restarted next frame
and cancelled the first back: neither preview ever arrived, and the server took a connect and an abort
per frame. `k_qv_inflight_max` is therefore one per quick view the app can show at once, which is three.
A fetch copies the `NetSite` rather than pointing at one, because a saved site is edited while its fetch
is running, and its connection belongs to the thread that opened it — `handle_cache()` is `thread_local`,
which is also why a background copy gets its own login. A binary file on this disk still gets its hex
dump; a row that is not on this disk does not, because pulling one down to find out is not what a preview
is for.

**Verified.** `test-remote`: 295 checks, 0 failures — 188 in a build without libcurl, where the
transport half is skipped and the parsers, the URL layer and the sites file still run. `ctest`: 4/4 in
both configurations. `SDL_VIDEODRIVER=offscreen timeout 3 ./build/mc-clone` stays up in both. `--version`
prints `remote sites: libcurl 8.22.0 (OpenSSL/3.6.5)`, or `remote sites: no FTP support in this build
(install libcurl and rebuild to browse FTP)`.

**Still open, and named.** L3's `curl_multi_*`, so a WAN *listing* does not freeze the frame loop.
Transfers and previews have left that problem — they run on worker threads now — but `net_list()` is
still called from the frame, with `CONNECTTIMEOUT_MS` and `LOW_SPEED_LIMIT`/`LOW_SPEED_TIME` there so a
dead server ends rather than hangs. The previews themselves are keyed by path, so a file the app rewrites
keeps previewing its old bytes until the entry is pushed out of the 32-deep cache; a local read costs
microseconds, so the cache is there for arrowing back and forth over a site, and the honest fix is to
invalidate an entry when the app is the one that wrote the file. The SFTP host-key UX: trust-on-first-use with the fingerprint remembered
in the sites file, still the weakest part of the feature. Two checks the testing paragraph above asks for
and does not have: a transfer the server stalls past `LOW_SPEED_TIME`, which needs the stall window
shortened for the test to run in seconds rather than twenty; and an assertion that `certificate not
verified` reaches the UI — the site list, the edit dialog and the status line all say it now, but nothing
checks that they do. The `version` test does not assert the FTP line either. One cost the parent-listing
`net_stat()` introduced: a recursive remote operation lists each directory twice, once for `net_stat()`
and once for the walk, which is worth collapsing when L3 lands.

## 9. Phase M — pane status bar (~1h, built)

**What it is.** One line under each pane's list: what the files in that folder add up to,
how many files there are, and how many directories. When entries are marked the counts turn
into `marked/total` and the size does the same, so the line answers *what am I about to copy*
without asking `Ctrl+L` first.

```text
/home/adam/github/immc                       /home/adam/downloads
  src  tests  samples  scripts                 alpha.tar.gz
  ...                                         beta.txt
12 files  3 dirs  4.2M                         2/9 files  1/2 dirs  8.1/64.4M
```

**Decisions.** Four things were open and are now fixed, because each one changes the answer
the line gives:

- **Folder only.** The byte total is the sum of the sizes of the *files listed in that folder*.
  A directory contributes its count and nothing to the bytes — its own `st_size` is an inode,
  not its content, and a recursive total is a tree walk the frame has no business starting.
  `Ctrl+L` (`count_marked_sizes` @6567) and `Alt+Shift+Enter` (`fill_dir_sizes` @6588) stay the
  way to get recursive numbers, and this line is what tells you they are worth running.
- **Visible rows.** The counts follow `pane.view`, so a quick filter that hides half the folder
  reports the half on screen. The line has to agree with the rows the user is looking at, or it
  is a second, contradicting source of truth.
- **Under the list.** One text row at the bottom of each pane's cell, above the command bar.
  It costs one row of list height per pane, which is the honest price and is paid once.
- **Always on.** No key, no config entry, no state. The line is short, it is derived, and a
  switch for it would be one more thing to remember and one more state to get wrong.

### 9.1 The data: `pane_totals()`

New in the selection-helpers block next to `marked_count` @191. Pure function of the pane, no
new fields on `Pane`:

```cpp
struct PaneTotals {
    long long bytes;          // file sizes over the visible rows
    int files, dirs;
    long long m_bytes;        // the same over the marked rows
    int m_files, m_dirs;
};

static PaneTotals pane_totals(const Pane& p) {
    PaneTotals t = {0, 0, 0, 0, 0, 0};
    for (size_t r = 0; r < p.view.size(); r++) {
        int i = p.view[r];
        if (i < 0 || i >= p.entry_count) continue;
        FileEntry* e = p.entries[i];
        if (!e || strcmp(e->name, "..") == 0) continue;   // the parent row is not content
        bool dir    = is_directory(e->name, e->mode);
        bool marked = i < (int)p.marked.size() && p.marked[i];
        if (dir) { t.dirs++; if (marked) t.m_dirs++; continue; }
        t.files++; t.bytes += e->size;
        if (marked) { t.m_files++; t.m_bytes += e->size; }
    }
    return t;
}
```

`view` is the only correct source: `entries` holds rows the filter hid, and the filter is the
one thing about a pane the user changed on purpose. `..` is excluded from both counts and the
bytes — it is a way out, not an entry in the folder. `marked` is guarded by size the way every
other reader of it already is (@4360, @4470).

**No cache, nothing to invalidate.** The numbers are derived from `view`, `entries` and
`marked`, all three of which `refresh_pane` @640-675 already rebuilds, and the walk is
O(visible rows) twice per frame. A cache here would only be a way to be stale.

### 9.2 The drawing: `render_pane_status()`

```cpp
static void render_pane_status(AppPanes* app, Pane& pane, bool focused, float width);
```

- Format, no marks: `12 files  3 dirs  4.2M`.
- Format, marks present: `2/9 files  1/2 dirs  8.1/64.4M` — marked first, always, so the
  number that changes is the one on the left.
- Nothing on screen: `(empty)`, and for a filtered pane `(no rows match)` rather than a bare
  `0 files 0 dirs`, which reads like the folder is empty when it is the filter that is empty.
- Sizes go through `human_size` @1025 so the line uses the same `4.2M` the size column uses.
- Inside an archive (`in_archive` @238) the bytes are the members' packed sizes, so the line
  says `4.2M packed`. A `.tar.gz` listing that reported 64 MB of members and 8 MB on disk
  would otherwise be the one number in this app that is quietly wrong.
- Inside a site (`in_remote` @264) the sizes are what `LIST` reported, which is the same number
  the size column already shows; directories are excluded from the bytes anyway, so the usual
  `0`/`4096` a server reports for a directory never reaches the total.
- Focused pane draws it with `TextUnformatted`, the unfocused one with `TextDisabled`, like the
  path bars above it.
- **Width.** This ImGui does not wrap `Text`/`TextDisabled` at the window edge (@2090), so the
  line is measured with `CalcTextSize` and, if it is wider than the pane, rebuilt in a compact
  form (`12f 3d 4.2M`) and finally truncated with an ellipsis. A narrow pane must not lose the
  marked subset, so the compact form drops the words before it drops the numbers.

### 9.3 The layout: one row out of the vertical budget

The budget at @5893-5908 is the only place a row can be taken from, and it has to be taken
before the table is laid out, not after:

```cpp
const float info_h = frame;                                   // the status row, per pane
const float left_child_h  = ImMax(2.0f * frame,
                                  base_h - (l_filter ? filt_h : 0.0f) - info_h);
const float right_child_h = ImMax(2.0f * frame,
                                  base_h - (r_filter ? filt_h : 0.0f) - info_h);
```

Then one call per column, after the list-or-quick-view branch, so all three list modes get it:

- after @5945 (`left_pane` / `left_qv`), before `ImGui::TableNextColumn()` @5947
- after @5980 (`right_pane` / `right_qv`), before `ImGui::EndTable()` @5982

Drawing it in the table cell rather than inside `render_file_list_body` @4305 is what makes
`TREE_EMBED` and `TREE_SPLIT` (@4585-4600) covered without touching either, and keeps the
`ctxmenu`/`panemenu` popups anchored to the list instead of to the new row.

The `ImMax` is new and is not decoration: the budget is already `win_h` minus five other rows,
and this feature takes a sixth. Clamped, a short window loses list rows and keeps the line;
unclamped, `BeginChild` gets a negative height.

`pane.page_size` is measured from the child window's height in `render_file_list_body` @4307,
so `PgUp`/`PgDn`/`Home`/`End` page one row shorter automatically. That is the whole cost of
the feature and it should be said out loud rather than discovered by someone who notices the
pane pages differently.

### 9.4 What the numbers mean in each list mode

| Mode | What the line reports | Note |
|---|---|---|
| `LIST_NORMAL` | the folder on screen | the intended case |
| `LIST_BRANCH` (`Ctrl+B`) | every row the branch view listed, i.e. the subtree | honest, and different from `LIST_NORMAL` on the same folder; the row count is the subtree's, so it must not be read as the folder's |
| `LIST_ARCHIVE` | the members of the current member directory | `packed` suffix; a member directory is not a tree total |
| `LIST_REMOTE` | the rows `LIST` returned | sizes are the server's own numbers |
| `TREE_EMBED` / `TREE_SPLIT` | the pane's *list* half, not the tree | the tree is a navigation model, not a listing |
| quick view panel (`F3`, `Ctrl+Q`) | still the pane's folder | the panel replaces the list, not the pane |

### 9.5 Tests

`test_pane_totals(&app, root)` — logic only, no display:

1. A folder with three files of known sizes and two directories: `files == 3`, `dirs == 2`,
   `bytes ==` the sum, and `..` in neither count nor total.
2. Mark one file and one directory: `m_files == 1`, `m_dirs == 1`, `m_bytes ==` that file's
   size only — a marked directory adds no bytes.
3. `pane.filter = "*.txt"` + `rebuild_view` @397: the totals follow the filtered rows, and the
   marked subset follows them too (a mark on a hidden row is not counted).
4. An empty directory and a directory holding only `..`: `(empty)` inputs, all totals zero.
5. Hidden entries: with `fs_set_show_hidden(false)` they are not in `entries` at all, so they
   are not in the totals; with it on they are. Asserted both ways, because this is where a
   "total" quietly disagrees with `du`.
6. Branch view (`Ctrl+B`) on `root/src`: the totals are the subtree's, and differ from the
   same folder in `LIST_NORMAL`.
7. An archive pane (the `pkg.zip` the suite already builds): member totals, and the packed
   wording.
8. A remote pane with no libcurl: the guard path — the line still draws, from the rows the
   listing produced.

`test_pane_status_row(&app, root)` — headless ImGui, the `test_menu_click` @3067 harness:

1. `ImGui::FindWindowByName("left_pane")->Size.y` is `info_h` shorter than the budget the
   layout computes for it, i.e. the row was actually paid for and not overlaid.
2. `pane.page_size` drops by exactly one row against the same folder measured before.
3. With a directory name long enough to overflow a 300 px pane, `CalcTextSize(line).x <=
   width`: the compact fallback runs instead of the line running off the edge.
4. The `test_ui_id_uniqueness` @1464 mouse sweep still reports zero conflicting item IDs with
   the new row in both panes.
5. Both panes show their own numbers, not the active pane's — the case a single shared bar
   would have got wrong for free.

Manual checklist additions:
1. `+`/`-`/`*`/`/` and `Ctrl+M` change the marked half of the line immediately, in both panes.
2. `Ctrl+S` filter narrows the numbers to the rows on screen; clearing it restores them.
3. `Tab`/`Ctrl+Tab`/`Alt+Left` into and out of a directory re-reads the line for the new folder.
4. `PgDn` pages one row shorter than before, and the line is still visible at the bottom.
5. A `.zip` pane says `packed`; a site pane does not.
6. A 400 px tall window keeps both lines and loses list rows instead of drawing a broken pane.

### 9.6 Deliberately not in this phase

- **Free space per pane** (`statvfs`), which is the other half of TC's panel status. It is a
  syscall per pane per frame and a second feature with its own questions (which mount, what a
  site or an archive has instead).
- **Recursive totals in the line.** `dir_size` @679 is already the cache for it and is filled
  lazily for exactly this reason; putting a tree walk behind a number that is on screen every
  frame is the same mistake `fill_dir_sizes` @6588 bounds to 200 per press.
- **A count of executables, or sizes by type.** TC does not show it and the line has no room
  for it.
- **Moving the existing `Marked: N  Clipboard: N` bottom bar.** It answers a different question
  (what the clipboard holds, which is cross-pane) and the two lines do not duplicate each other
  once the pane line carries the marked subset.

### 9.7 Built, verified, and where the plan was wrong

**Built.** `struct PaneTotals` and `pane_totals()` in the selection-helpers block next to
`marked_count`; `human_size_trim()` beside `human_size`; `pane_status_parts()`,
`pane_status_text()` and `render_pane_status()` after `render_filter_row`; `info_h` in the
vertical budget with the `ImMax` clamp; one call per table column, after the list-or-quick-view
branch on each side.

**Three places the plan was changed while building it, each for a reason.**

- The line is assembled from three parts rather than written as two whole formats, because a
  category only splits into `m/t` when it has something to split. With a mark on a file and none
  on a directory, `1/2 files  0 dirs  6/12` is the whole truth and `0/0 dirs` is noise that makes
  the line harder to scan.
- `human_size` pads to four columns so the size column right-aligns. In a sentence that padding
  reads as a hole in the middle of it — `3 files  1 dirs    18` is the same fact written worse —
  so the row goes through `human_size_trim`, which is `human_size` with the leading spaces moved
  out. The size column keeps its padding.
- `render_pane_status` takes no `AppPanes*`. The row needs the pane and nothing else, and an
  unused parameter is a promise that the row will grow a dependency it does not have.

**The layout assertion the plan asked for is not measurable, and what replaced it.** The plan
asked for `FindWindowByName("left_pane")->Size.y` compared against the budget. Neither half of
that works. `BeginChildEx` names a child `Midnight Commander/left_pane_<ID>`, not `left_pane`, so
the lookup finds nothing; and a child window's `Pos` is not a usable measure of where it draws in
this layout — the left pane's child reports `Pos.y = -250` and the right one `366` for the same
table row, and it reports exactly the same pair with the `info_h` reservation removed. That is a
pre-existing property of a table whose cells hold child windows, not something this change
introduced, and an assertion built on it would have passed or failed for reasons nothing to do
with the feature. `Size.y` is exact — 567 with the row reserved, 590 without — so the test runs
the budget chain backwards out of the window the pane was actually given and requires the result
to land just under the menu bar. Removing `info_h` makes it fail in both panes, which is the
property an assertion about a reserved row has to have.

**Verified.** `test-panes`: 1773 checks, 0 failures, 43 of them new. `ctest`: 4/4. `mc-clone`
built and `SDL_VIDEODRIVER=offscreen timeout 3 ./build/mc-clone` stays up. The new checks cover:
counts and bytes over a folder of known sizes; `..` excluded from both counts and the total; a
marked directory adding a count and no bytes; the filter narrowing the totals, and a mark on a
hidden row not counted; `(empty)` against `(no rows match)`; hidden entries counted exactly when
they are listed; branch view describing the subtree and the plain list describing the folder; an
archive's members with `packed`; each pane reporting its own folder; the width fallback firing and
keeping the marked subset; `page_size` matching the window the pane was given; and the budget
chain in both panes.

**Not done from the plan, and named.** The remote-pane case: `test-panes` has no libcurl
guarantee and the FTP fixture belongs to `test-remote`, so the remote row is covered only in the
sense that it is the same code over rows a listing produced — the sizes a server reports for a
directory never reach the total because directories contribute nothing, which is the reason the
case needed no special handling. The ID sweep here is a coarse pass over the bottom 90 px of the
window rather than the full-window sweep `test_ui_id_uniqueness` already runs, which covers the
same strip.
