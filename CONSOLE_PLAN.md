# Console — replace the command line with a hotkey console window

Status: **built.** Every phase below landed, with the differences listed in §13.
`Ctrl+Enter` opens the console over either pane; `Esc` hides it and the backlog
stays. `test-panes` runs 1878 checks with 0 failures, of which `test_console_keyboard`
and `test_console` are this feature.

Replaces the bottom command line of `mc-clone` with a console window that opens on
`Ctrl+Enter` in either pane and hides on `Esc`, keeping its transcript.

---

## 1. The shape

There is no command line. There is one window:

```
┌ Console ─────────────────────────────────── ~/github/immc ─┐
│ $ git log --oneline | head -3                               │
│ cadca00 Make the command line a text field when you click  │
│ e32109e Show each pane's sizes, file and directory counts  │
│ c453d0e Make F10 toggle the Commands menu every time       │
│                                                            │
│ $ ls build                                                 │
│ CMakeCache.txt  mc-clone  test-panes                       │
│ exit 0                                                     │
│                                                            │
│ $ find / -name mc-clone              running… Esc cancels  │
│                                                            │
│ │ls -la build/                                             │
└────────────────────────────────────────────────────────────┘
```

- **`Ctrl+Enter`** opens it, in whichever pane has the focus. Nothing else in the
  keymap uses it (`app_key_is_special` @6810 already routes `SDLK_RETURN` to
  `app_panes_handle_key`, and the Enter branch @7539 ignores the modifier, so
  `Ctrl+Enter` today is just a plain Enter — the binding is free).
- **`Enter`** runs the line. **`Esc`** hides the window and **keeps the backlog**.
- The backlog is not the last command's output. It is the whole transcript of the
  session — every command and its output, oldest first — and it survives hiding the
  window, navigating a pane, changing tabs, entering an archive or a site, and the
  process: it is written to the config file on exit and read back on start.
- Commands run in **the active pane's directory**, not the directory the app was
  launched from. That is what makes "reopen it in another directory" useful: the
  history is still there, the working directory has moved.

`Ctrl+C` is not available — it is the file clipboard's copy (@7374). `Ctrl+Enter`
is the free key that reads like a terminal.

---

## 2. Decisions

| Point | Decision | Why |
|---|---|---|
| Hotkey | `Ctrl+Enter` (and `Ctrl+KP_Enter`) | Free. `Ctrl+C/X/V` are the clipboard, `Ctrl+J/K/G/\`` are free but collide with readline habits inside a console, where `Ctrl+K` is kill-line and `Ctrl+C` is interrupt. |
| Backlog lifetime | Session **and** across restarts, through `~/.config/mc-clone/config` | The stated point of the feature. Indexed-key persistence already exists for pane tabs (`left_tab%d` @config.cpp:195). |
| Execution | Child process on a `std::thread`, output appended as it arrives, `Esc` cancels | A `find /` must not freeze the UI. `QvFetch` @panes.h:194 and `BgTransfer` @panes.h:160 already define the ownership rules to copy. |
| Process plumbing | Reuse `proc_start`/`proc_finish` @archive.cpp:229/265 via a new `src/proc.cpp` | They already do the four things this needs: pipe stdout+stderr into one stream, `/dev/null` on stdin so a password prompt fails instead of hanging, `chdir(cwd)` in the child, and SIGTERM→SIGKILL cancel with an exit code. `popen` gives none of the last two. |
| Working directory | The active pane's `current_dir`, passed as `proc_start`'s `cwd` | The console is a file-manager console. The pane it was opened from is the directory it means. |
| Shell | `/bin/sh -c "<line>"` | Pipes, redirection and globbing come free. `execvp` on the first word would lose all three. |
| Builtins | `cd`, `exit`/`quit`, `pwd`, `clear`, `history` | `cd` must move a pane, not the app process — the app's own cwd is not where the panes are. Keep `exec_command`'s @1123 `cd` logic verbatim: `~`, absolute, relative-to-pane, and `net_is_remote` → `remote_open_url`. |
| Old bindings | `Ctrl+P`, `Alt+C`, `Alt+F8` move into the console | They are all "get text into a command". With no command line they have nowhere else to go. |
| Bottom bar | Stays, as a pure status line | It carries `status_msg`, the mark count, the clipboard and the unverified-certificate warning @6136-6160. Only the `$` field leaves. |

---

## 3. Data model (`src/panes.h`)

```cpp
// One console entry: what was typed, where it ran, what it printed.
struct ConsoleEntry {
    std::string cwd;              // the pane directory the command ran in
    std::string cmd;              // the line as typed
    std::string out;              // stdout+stderr, appended as it arrives
    int  code = -1;               // exit code; -1 while running
    bool running = false;         // worker still feeding `out`
    bool builtin = false;         // cd/exit/pwd/clear: no child process
};

// Console window. Opened by Ctrl+Enter, hidden by Esc, never destroyed:
// `log` is the backlog, and it outlives the window.
bool         console_open;          // the window is on screen
bool         console_field_active;  // its input owns the keyboard this frame
char         console_input[512];    // the line being typed
std::vector<ConsoleEntry> console_log;   // oldest first
std::vector<std::string>  console_hist;  // commands only, for Up/Down recall
int          console_hist_pos;      // == size() means "editing a fresh line"
std::mutex   console_mu;            // guards the running entry's `out`
pid_t        console_pid;           // >0: a child is running
int          console_w, console_h;  // remembered size, like qv_w/qv_h
bool         console_scroll_lock;   // the user scrolled up; stop following the tail
```

Caps, all of them enforced where the data is appended, not where it is drawn:

| Cap | Value | Reason |
|---|---|---|
| Transcript entries | 200, oldest dropped | A session of `ls` must not grow without bound. |
| Output per command | 256 KB, with a `… output truncated` line | `cat` of a large file is the realistic worst case. |
| Persisted entries | last 50, 8 KB of output each | The config file stays a file a person can read. |
| History | 200 commands | `cmd_history[64][512]` today is too small for a backlog feature. |

`output_open` / `output_text[64*1024]` @panes.h:302 and `render_output` @4727 are
deleted: the transcript replaces them.

---

## 4. Keyboard routing

### 4.1 Who owns the keyboard while the console is open

The console is a window, not a modal. The rule is the one the command line already
uses for clicks (`cmd_field_claimed` @panes.h:299), expressed as a claim:

| State | Keys go to |
|---|---|
| `console_open` **and** `console_field_active` (opened by `Ctrl+Enter`, or clicked into) | The console. `Esc` hides, `Enter`/`Ctrl+Enter` runs, `Up`/`Down` walk history when the line is empty, everything else — typing, caret keys, `Ctrl+A/C/V/X/Z` — goes to `ImGui_ImplSDL3_ProcessEvent`. |
| `console_open` and the field does **not** own the keyboard (a click landed on a pane row, a menu, a path bar) | Normal routing. The panes keep working, the console stays on screen as a passive transcript, `Ctrl+Enter` takes the keyboard back. |

The first row is the win: forwarding every non-`Esc`/`Enter` key to ImGui while the
field holds the keyboard gives the console real text editing — `Ctrl+C`, `Ctrl+V`,
`Ctrl+A`, `Ctrl+Z`, `Ctrl+←` word-jump — which the command line has never had,
because `app_key_is_special` @6885 hands those combinations to the panes.

New branch in `main.cpp`, placed **after** the confirm/op/path-bar/search branches
and **before** the `app_key_is_special` fallthrough @245:

```cpp
// The console owns the keyboard while its field holds it: Esc hides, Enter runs,
// Up/Down recall history, and every other key — including the Ctrl editing
// combinations — belongs to the text field.
if (panes->console_open && panes->console_field_active) {
    if (k == SDLK_ESCAPE || k == SDLK_RETURN || k == SDLK_KP_ENTER ||
        ((k == SDLK_UP || k == SDLK_DOWN) && !(m & (SDL_KMOD_CTRL | SDL_KMOD_ALT))))
        app_panes_handle_key(panes, k, m);
    else
        ImGui_ImplSDL3_ProcessEvent(&event);
    continue;
}
```

### 4.2 `Ctrl+Enter` precedence

`Ctrl+Enter` is swallowed by four branches that treat any `RETURN` as "accept".
That is the right behaviour and gets documented instead of worked around:

| While this owns the keyboard | `Ctrl+Enter` does |
|---|---|
| Confirm dialog (`confirm != CONF_NONE` @main.cpp:203) | Answers yes, as `Enter` does |
| Op dialog F5/F6/F7/F2/Shift+F4 (@main.cpp:211) | Runs the dialog's focused button |
| A pane's path bar (`active_path_pane >= 0` @main.cpp:222) | Commits the path |
| Quick filter field (`filter_field_active`) | Commits the filter |
| Nothing (the normal case) | Opens the console |

In `app_panes_handle_key`, the open binding goes **before** the Enter branch @7539
and after the window-owns-the-keyboard block @6999-7041:

```cpp
if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && (mod & SDL_KMOD_CTRL) &&
    !(mod & (SDL_KMOD_ALT | SDL_KMOD_MODE))) {
    if (!app->console_open) { app->console_open = true; app->console_focus_request = true; }
    else if (app->console_field_active) console_run(app);   // Ctrl+Enter == Enter
    else app->console_focus_request = true;                 // bring the keyboard back
    return;
}
```

`console_focus_request` is the one-frame flag that calls `ImGui::SetKeyboardFocusHere()`
for the console's `InputText` on the next frame — the same deferred-focus rule the
command line learned the hard way (@6166-6180: asking for focus while a press is in
flight drops the click that was in flight).

### 4.3 `Esc` order

`Esc` already has a queue @7556. The console joins it after the menus and the quick
filter, before the status clear:

1. a keyboard-opened menu closes,
2. a quick filter clears,
3. **the console hides and keeps its backlog**,
4. `status_msg` clears.

While a command is running, the first `Esc` cancels the child (`SIGTERM`, then
`SIGKILL` 1 s later — `proc_finish(aborted=true)` @archive.cpp:265) and the second
hides the window. A cancel is recorded in the transcript as `interrupted`, so the
backlog says what happened.

### 4.4 What the removal gives back

Deleting `app->cmd` deletes the ambiguity the README has to explain @150-157. Every
`app->cmd[0] == '\0'` test in `app_key_is_special` @6860-6885 becomes unconditional:

| Key | Today | After |
|---|---|---|
| `Space` | marks only when the command line is empty | always marks |
| `Backspace` | parent dir only when empty | always parent dir |
| `/` `+` `-` `*` | marking shortcuts only when empty | always marking |
| `Ctrl+C/X/V` | clipboard only when the command line is empty | always clipboard |
| `Ctrl+A` | select-all marks | unchanged, and `Ctrl+A` inside the console selects the text |

`cmd_field_active` / `cmd_field_claimed` @panes.h:298-299 and the `cmd_claimed`
lambda @6846 go away with the field.

---

## 5. Execution engine

`proc_start`/`proc_finish` @archive.cpp:229-281 move to a new `src/proc.{h,cpp}`
(`archive.cpp` keeps using them, `CMakeLists.txt` gains the source in both the
executable and `test-panes`), and the console drives them from a worker:

```cpp
struct ConsoleJob {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::string cwd, cmd, out;      // `out` is guarded by AppPanes::console_mu
    int code = -1;
    std::thread worker;
};
```

- **Start.** Append a `ConsoleEntry{cwd, cmd, "", -1, true}` to `console_log`, push
  the command onto `console_hist`, then start the worker: `proc_start({"/bin/sh",
  "-c", cmd}, cwd, kNoEnv, &p, &err)`. A `proc_start` failure (no `sh`) is recorded
  as an entry with the error text and code 127 — the same code `proc_start`'s child
  uses when `execvp` fails.
- **Live tail.** The worker reads the pipe in 4 KB chunks, sanitises them (§7), and
  appends under `console_mu`. The UI thread copies the tail under the same mutex
  each frame. No UI thread ever blocks on a child.
- **Finish.** The worker sets `code` from `proc_finish(&p, cancel)`, then `done =
  true`. The UI thread's reap — the `qv_reap` pattern @panes.cpp:4150 — sees `done`,
  writes the `exit N` line, clears `running`, and joins. Only a finished worker is
  ever joined, and the join happens before the entry leaves the vector.
- **Cancel.** `Esc` sets `cancel`; the worker calls `proc_finish(&p, true)`.
- **One at a time.** `console_pid > 0` means a child is running: `Enter` on a new
  line is refused with a status message rather than starting a second one.
- **Builtins** run on the UI thread with no child: `cd` (the `exec_command` @1123
  body, unchanged, including `remote_open_url`), `exit`/`quit` (`quit_request`),
  `pwd`, `clear`, `history`.
- **Between frames, not inside one.** `cd` moves a pane, which refreshes it, which
  must not happen while ImGui is mid-frame. Builtins that navigate set
  `pending_op = DEFER_CONSOLE` and `app_panes_run_pending` @main.cpp:275 runs them,
  which is exactly how the menus already defer a copy.

---

## 6. Rendering

`render_console(app)` in the floating-window list @6200, next to `render_viewer`.

- `ImGui::Begin("Console", …, ImGuiWindowFlags_NoCollapse)`, size from
  `console_w/console_h` with `ImGuiCond_FirstUseEver`, position remembered the same
  way the quick view remembers its size (`quickview_w/h` @config.h:16) — the user
  moved it once, it stays there.
- Transcript: one `ImGui::BeginChild` holding everything above the input,
  `ImGuiChildFlags_Borders`, scroll Y. Each entry draws `$ <cmd>` in the dim colour,
  then the output as one `TextUnformatted` block in the mono font, then `exit N` in
  the dim colour. A running entry draws a `running…` marker and its partial output.
- Input: `SetKeyboardFocusHere()` on the frame after `console_focus_request`,
  `SetNextItemWidth(-1)`, `InputText("##console", console_input, …)`, then
  `console_field_active = ImGui::IsItemActive()`.
- Tail following: auto-scroll to the bottom while `console_scroll_lock` is false;
  any scroll-up while a command is running sets it, and the next `Ctrl+Enter` or a
  scroll back to the bottom clears it. Without this a `find` pushes the line you are
  typing off screen.
- The header shows the directory the next command will run in — the active pane's
  `current_dir`, or `archive::virtual path` when the pane is inside an archive or on
  a site, where a shell command has no meaning. In those panes the input is disabled
  and the header says why, rather than running the command in the wrong directory.
- History: `Up`/`Down` with an empty line walk `console_hist`; with text in the line
  they belong to the caret. `Alt+F8` opens the history as a list inside the window
  (`render_cmdhist` @4803 moves here).

---

## 7. Output hygiene

- Sanitise on append, not on draw: keep `\n` and `\t`, drop `\r`, replace control
  bytes and invalid UTF-8 with `·`. ImGui's vertex builder is not a place to discover
  that a command printed a progress bar full of `\r` and bell characters.
- Per-line wrapping is ImGui's job (`TextUnformatted` wraps); no manual wrapping.
- A command that wants a TTY (`vim`, `ssh` password, `read`) gets `/dev/null` on
  stdin from `proc_start` and fails immediately with its own error in the transcript
  — the same reasoning as the archive runner's comment @archive.cpp:244 and the
  `zip -P` note @archive.cpp:963. `F4` already shows the alternative: fork a terminal
  emulator so the tool gets a TTY @3623.
- Secrets: the transcript is plain text in `~/.config/mc-clone/config`. `clear`
  before exiting, or `console_save = 0` in the config, or `console_save = 0` in the
  Options dialog. The sites file is `0600`; the config file is not, and a typed
  `curl -u user:pass` lands in it.

---

## 8. Removal list

| Site | Action |
|---|---|
| `AppPanes::cmd`, `cmd_history[64][512]`, `cmd_hist_count/pos`, `cmd_field_active`, `cmd_field_claimed` @panes.h:287-299 | delete; the console's own fields replace them |
| `output_open`, `output_text[64*1024]` @panes.h:302, `render_output` @4727, its call @6204 | delete; the transcript replaces them |
| Bottom-bar `InputText("##cmdline")` block @6131-6190 | delete the `$` label and the field; keep the separator, `status_msg`, the mark/clipboard/cert info @6136-6160 and the `allow_cmd_focus` reasoning, which now guards nothing and goes |
| `bar_h = frame + 8.0f` @6022 | retitle to "status bar"; the vertical budget is unchanged, the row is still needed |
| `exec_command` @1112 | becomes `console_run(app)` reading `console_input`; `run_shell_and_show` @1090 is deleted (the worker replaces it) |
| `app_key_is_special` @6860-6885 | drop the `cmd[0] == '\0'` tests and the `cmd_claimed` lambda; add nothing (Ctrl+Enter is already `special`) |
| Enter branch @7539-7554 | drop `if (app->cmd[0] != '\0') exec_command(app)`; add the `Ctrl+Enter` binding above it |
| Esc branch @7576-7578 | drop `app->cmd[0] = '\0'` and `cmd_hist_pos`; insert the console hide |
| `cmd_put_path` @3248 (`Ctrl+P`), `Alt+C` @7263, `Alt+F8` @7161 + `render_cmdhist` @4803 | retarget to `console_input` / the console window; `Ctrl+P` and `Alt+C` open the console if it is closed |
| Help text @5027 (`$ command line …`), menu item @5915 (`Current path to command line`) | reword for the console |
| `tests/test_panes.cpp` @694-698, @748, @1037-1062, @1282, @3463 | retarget to `console_input` / `console_log`; the `Ctrl+P` truncation test stays, against the new buffer |
| `README.md` @150-157 (the claim paragraph), @199-201 (`Alt+F8`, `Alt+C`), the keymap tables | rewrite for `Ctrl+Enter` |
| `FEATURES_PLAN.md` @35, 71-79, 159, 235, 303, 427, 476, 591, 598, 645-646, 674, 685 | annotate as superseded by this file |

---

## 9. Phases

Each phase leaves the app working; the command line is not deleted until the
console can do everything the command line did.

| # | Work | Effort |
|---|---|---|
| 1 | **Console window skeleton.** Fields in `panes.h`, `render_console`, `Ctrl+Enter` open / `Esc` hide, the main.cpp ownership branch, `console_focus_request`, remembered size in `MCConfig`. Execution still the blocking `run_shell_and_show`, output appended to `console_log`. | ~2 h |
| 2 | **Execution moved to a thread.** `src/proc.{h,cpp}` extraction from `archive.cpp`, `ConsoleJob`, live tail under `console_mu`, reap-and-join, `Esc` cancel, per-command output cap, sanitiser. | ~3 h |
| 3 | **Builtins and cwd.** `cd`/`exit`/`pwd`/`clear`/`history`, `DEFER_CONSOLE`, `cwd` = active pane's `current_dir`, archive/site panes refuse shell commands with a reason. | ~1.5 h |
| 4 | **Delete the command line.** §8 in one pass: bottom bar, `app_key_is_special`, Enter/Esc branches, `Ctrl+P`/`Alt+C`/`Alt+F8`, tests, README. | ~2 h |
| 5 | **Backlog persistence.** Escaped `console_cmd<i>` / `console_out<i>` keys in `config.cpp`, caps, `console_save` switch, round-trip test. | ~1 h |
| 6 | **History recall.** `Up`/`Down` on an empty line, `Alt+F8` list inside the window, 200-entry history. | ~1 h |

Total ≈ 10.5 h. Phase 4 is the one that changes the feel of the app; phases 1-3 are
additive and can be shipped without it.

---

## 10. Verification

New `test_console` in `tests/test_panes.cpp` (the harness already drives
`app_panes_handle_key` directly):

- `Ctrl+Enter` opens; `Esc` hides; `console_log` is unchanged by either.
- `navigate_to`, `swap_panes`, `tab_new` and an archive entry do not touch `console_log`.
- `cd build` moves the focused pane and is recorded as a builtin entry; `cd ftp://…`
  still reaches `remote_open_url`; `cd /nope` records the error line.
- `Ctrl+P` appends the pane path, keeps existing text, inserts the missing space, and
  truncates a 4096-byte path inside the 512 buffer (the existing @1055 assertions).
- A real command (`echo`) through the worker lands in `console_log` with `code == 0`;
  a failing one records `128 + signal` or its exit code.
- A command started in `/a` and a second in `/b` record different `cwd` values in the
  same transcript.
- Transcript cap: 250 appends leave 200 entries, oldest gone. Output cap: a 1 MB
  `echo` is truncated with the marker line.
- `mc_config_save` → `mc_config_load` round-trips a transcript containing newlines,
  tabs, `=`, `#` and a non-UTF-8 byte.
- `console_save = 0` writes nothing.
- The migrated `Ctrl+P` / archive `Ctrl+P` tests pass against `console_input`.

Manual: `Ctrl+Enter` from each pane; `find /` with `Esc` mid-run and the transcript
saying `interrupted`; `Ctrl+V` pasting a path into the console field; `Space` marking
a file with the console open but not holding the keyboard; reopening the app and
finding yesterday's transcript.

---

## 11. Risks

| Risk | Handling |
|---|---|
| `Ctrl+Enter` is eaten by the confirm/op/path-bar branches | Accepted and documented (§4.2). Those fields need Enter; the console does not need to interrupt them. |
| Two threads writing the transcript | Only the worker writes `out`, under `console_mu`; `console_log` structure is touched by the UI thread alone. |
| A worker outlives its entry | The reap joins only `done` workers, before the entry is erased — the `BgTransfer`/`QvFetch` rule @panes.h:154. |
| `Ctrl+Enter` on a keyboard without a separate Enter | `KP_Enter` is accepted too; `Ctrl+M` is the same keycode as `Ctrl+Enter` on most layouts, which is harmless. |
| Transcript in a world-readable config file | `console_save` switch + `clear` builtin + a line in the README. |
| A command that never exits | Cancel is always available; no timeout is invented. |
| Losing `cd ftp://…` | `exec_command`'s `cd` body moves to the console verbatim, and `remote_open_url` stays the single entry point for a typed URL. |

---

## 12. Out of scope

No PTY, so no interactive TTY program inside the console (`vim`, `ssh` password
prompts, `less`) — `F4`'s terminal-emulator fork @3623 is the answer for editors.
No shell state between commands: each command is a new process, so `export`,
`alias`, and a shell `cd` do not persist — the `cd` builtin is the substitute. No
tab completion, no glob expansion of our own (the shell does it), no command
substitution UI, no per-pane consoles (one transcript, one window), no scrollback
beyond the caps in §3.

---

## 13. What changed while it was built

| Planned | Built | Why it moved |
|---|---|---|
| `DEFER_CONSOLE` for builtins that navigate | no deferral | `app_panes_handle_key` runs in the SDL event poll, **before** `ImGui::NewFrame()`, so `cd` → `navigate_to` → `refresh_pane` is already outside a frame. `Enter` on a directory has always done exactly that. An enum value with no caller is dead code, so it was not added. |
| Enter on an executable keeps running the tool in place | it runs through the console | `activate_entry`'s executable branch used `run_shell_and_show`, which is the blocking popen this feature deletes. Its output belongs in the transcript and its run belongs on the worker, so `console_start` is shared: `console_run` is the half that decides a line is a command, `console_start` is the half that starts one. |
| `console_save` in the Options dialog | a line in the config file only | What it protects has to be switchable without opening a window and without a dialog row for a feature most sessions never use. The file documents it. |
| Output cap applied when the buffer *exceeds* the cap | applied when it reaches the cap | A 4 KB chunk that landed exactly on 256 KB stopped the output short without ever saying it had. Found by the cap test. |
| Transcript entries carry a `running` flag | `console_job != nullptr` means the last entry is running | One fact, stored once. A vector element shared with a worker is the kind of thing that turns into a race later. |

Everything else in this file is what shipped: `Ctrl+Enter`, the ownership rule in
`main.cpp`, `proc_start`/`proc_finish` extracted to `src/proc.{h,cpp}` and shared
with the archive backends, the worker with `poll()` so a cancel lands while a tool
is silent, the sanitiser on append, the caps, the escaped config keys, the history
list inside the window, and the removal of the command line's `cmd[0] == '\0'`
conditions from `app_key_is_special`.

**The rule the first run found: the field owns its buffer.** From the moment an
`InputText` has the keyboard, ImGui edits its own copy of the buffer and displays
that copy; the app's writes to the buffer are ignored until the field is validated
or deactivated. So every place the app writes the console's line - `Enter` clearing
it, `Up`/`Down` recalling one, `Ctrl+P` appending a path, `Alt+C` prefilled `cd `, a
history pick - needed a reload request, not an assignment. `console_set_input()` is
the only writer of `console_input` outside the field, and it sets
`console_input_reload`, which `render_console` answers with
`ImGui::GetInputTextState(ImGui::GetID("##console"))->ReloadUserBufAndMoveToEnd()`
before drawing the field. Without it the user types a command, presses `Enter`, the
command runs, and the command is still sitting in the box - which is what the first
run reported. `Enter` is deliberately not handed to ImGui, so the field never
deactivates and the console stays ready for the next line; that is also why the bug
could not fix itself by losing focus.

The test for it asserts the field's own editing state, not just the app's buffer:
`ImGui::GetInputTextState(ImGui::GetActiveID())->TextA` is what the box shows. A
test that only reads `console_input` passes with the bug in place, which is how the
bug survived the first round - `console_type()` wrote the buffer directly and never
typed into a live field.
