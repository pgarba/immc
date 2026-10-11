# immc

[![build and test]][ci]

A dual-pane file manager for the keyboard. Two file panes, a command line, and the
file operations under the F-keys, in the shape Total Commander and Midnight
Commander trained people on: `F5` copies, `F6` moves, `F7` makes a folder, `F3`
previews, `Tab` switches panes, `Space` marks, `Insert` selects.

It is not a terminal program. It opens a window and draws a table, using Dear
ImGui over SDL3 and OpenGL 3. It runs on Linux.

The binary it builds is still called `mc-clone`; `immc` is the project, the desktop
entry and the window title.

## Build

Needs CMake 3.20 or newer, a C++20 compiler, SDL3 (with its CMake config) and an
OpenGL 3 implementation. Two optional dependencies change what the app can do:

| dependency | what it turns on |
| --- | --- |
| libcurl | FTP, FTPS and SFTP: browsing sites, and copy/move that runs on a thread |
| FreeType | the font hinting modes the Options dialog offers |

On a Debian-ish box:

    apt install cmake g++ libcurl4-openssl-dev libfreetype-dev

SDL3 is the one dependency that may not be packaged. Ubuntu 24.04, which is
what GitHub's `ubuntu-latest` runner is, has no `libsdl3-dev` at all. Check
with `apt-cache policy libsdl3-dev`, and if it is missing, build it from the
release tarball the way CI does:

    curl -fsSLO https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    tar xzf SDL3-3.4.18.tar.gz
    cmake -S SDL3-3.4.18 -B sdl3 -DCMAKE_BUILD_TYPE=Release -DSDL_TESTS=OFF
    cmake --build sdl3 -j4
    sudo cmake --install sdl3

SDL3 needs X11 development headers before it will configure, and it treats its
X11 sub-modules as required rather than optional: a missing `libxcursor-dev`
stops configure instead of quietly dropping cursor support. So the list is
longer than it looks:

    libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev \
    libxrandr-dev libxss-dev libxtst-dev libxrender-dev libxkbcommon-dev

Add `libasound2-dev` and `libpulse-dev` for audio. `.github/workflows/ci.yml`
installs exactly that set, which is the quickest way to check the list is still
correct. Installing it elsewhere works too; point CMake
at the prefix with `-DCMAKE_PREFIX_PATH=/path/to/prefix`.

Dear ImGui is not vendored in this repository. Fetch the pinned copy first:

    scripts/get-deps.sh

That clones `ocornut/imgui` to the exact commit this project is built and tested
against (`v1.92.9b-112-gf502f9c98`) into `extern_dont_commit/imgui`, which is where
`CMakeLists.txt` looks for it. The commit is pinned rather than "latest" because
ImGui's master carries a `WIP` version string and moves, so an unpinned clone can
break a build whose own source never changed.

Then:

    cmake -S . -B build
    cmake --build build -j4
    ./build/mc-clone

A build **without** libcurl is a supported configuration rather than a broken one.
The URL layer, the sites file and both FTP listing parsers still compile and are
still tested; only the transport answers that this build has no FTP support, and
`--version` and the About window say which build you are looking at.

## Tests

    cd build && ctest

Four suites, all headless, about 2,200 checks between them:

| suite | what it covers |
| --- | --- |
| `panes` | the pane logic: marking, selection, sorting, filtering, tabs, directory history, the operation dialogs and their keyboard ring, quick view, background transfers, config round-trips through the real file. It `#include`s `src/panes.cpp` so the file-static helpers can be exercised directly. |
| `archive` | the archive backends, against real `zip`/`tar`/`7z` files that the test builds with the same tools the app shells out to |
| `remote` | the URL layer, the sites file and its `0600` mode, the MLSD and LIST dialects against recorded server output, and then the transport against a real FTP server the test starts on `127.0.0.1` (`tests/ftp_server.py`) |
| `version` | `mc-clone --version`, which runs before SDL is initialised |

The remote suite skips its network half when the build has no libcurl or the
fixture will not start, and runs the parsers either way. The no-libcurl
configuration is verified as a first-class build, not assumed:

    cmake -S . -B build-nocurl -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON
    cmake --build build-nocurl -j4
    (cd build-nocurl && ctest)

`.github/workflows/ci.yml` runs exactly those two configurations on every push, so
the bootstrap script and the no-libcurl build cannot quietly rot.

## Keys

The full reference is in the app (`F10` for the menus, `F1` for the About window)
and in `FEATURES_PLAN.md`. The ones worth knowing on the first run:

### Around

| | |
| --- | --- |
| `Tab` | switch pane |
| `Ctrl+Tab` | cycle the tabs in a pane |
| `Alt+←` `Alt+→` | back and forward through the pane's visited directories |
| `Alt+↓` | the list of directories this pane has been in |
| `Alt+F11` `Alt+F12` | edit the left / right path bar |
| `Ctrl+←` `Ctrl+→` | open the directory under the cursor in the other pane |
| `Alt+F1` `Alt+F2` | the volumes popup, read from `/proc/mounts` |
| `Shift+Tab` | toggle the split tree panel |

### Files

| | |
| --- | --- |
| `Enter` `Right` | open |
| `Backspace` `Left` | up a directory |
| `F3` | quick view: the beginning of the file under the cursor, including one on a site or inside an archive |
| `Ctrl+Q` | quick view as a pane, instead of a file pane |
| `Alt+F3` | open with the desktop's default program |
| `F4` | edit in the terminal editor |
| `F5` `F6` | copy, move — a dialog first, and `Tab` walks it |
| `F7` | new folder |
| `Shift+F4` | new text file, then edit it |
| `F2` | rename |
| `F8` | delete to trash · `Shift+F8` delete for good |
| `Insert` `Space` | select · mark |
| `Ctrl+Num+` `Ctrl+Num-` | mark all · none |
| `Alt+Num+` `Alt+Num-` | mark · unmark everything with the cursor's extension |
| `Ctrl+C` `Ctrl+X` `Ctrl+V` | the app's own file clipboard |
| `Ctrl+L` | total size of the marked entries |
| `Alt+Shift+Enter` | fill in the size of every directory |
| `Alt+Enter` | properties |

Each pane ends with a status row: what the files on screen add up to, how many
files there are, and how many directories. With marks it becomes
`2/9 files  1/2 dirs  8.1/64.4M` — marked first — so the row answers *what am I
about to copy* without asking `Ctrl+L` first. The bytes are the files in that
folder: a directory contributes its count and no size, which is what `Ctrl+L` and
`Alt+Shift+Enter` exist for. The counts follow the quick filter, so they describe
the rows you can see, and in branch view they describe the subtree branch view
listed. Inside an archive the sizes are the members' sizes as the archive lists
them, and the row says `packed`.

The command line at the bottom holds the keyboard whenever nothing else has it,
which is why “the box is focused” cannot decide whether `Space` marks a file or
types a space. Clicking into the box decides it. While you hold it there, `Space`
`Backspace` `Del` `/` `+` `-` `*` are typing, so a space as the first character
arrives as a space instead of marking the cursor’s file and the `Enter` after it
acts on nothing. `Enter` itself is unchanged: an empty command line still opens
the file under the cursor. Hand the keyboard to anything else — `Ctrl+S` gives it
to the filter — and the shortcuts come back.

### Tabs

| | |
| --- | --- |
| `Ctrl+T` `Ctrl+Shift+T` | new tab, activated · not activated |
| `Ctrl+W` `Ctrl+Shift+W` | close this tab · close all but one |
| `Ctrl+↑` `Ctrl+Shift+↓` | open the cursor in a new tab here · in the other pane |
| `Ctrl+U` | swap the panes' directories; with `Shift`, swap the whole tab sets |

### Looking

| | |
| --- | --- |
| `Ctrl+F3`…`Ctrl+F7` | sort by name, extension, date, size, unsorted |
| `Ctrl+S` | quick filter; `Ctrl+F10` clears it, `Ctrl+F11` is executables only, `Ctrl+F12` is the saved filter |
| `Ctrl+B` | branch view: the subtree flattened into the pane |
| `Ctrl+F8` `Alt+F10` | the directory tree |
| `Shift+F1` `Ctrl+F1` | the columns view · the brief one |
| `Shift+F2` | compare the two directories, read-only |
| `Alt+.` | show hidden files |
| `F9` | the per-pane user menu |
| `F10` `Shift+F10` | the menu bar · the context menu for the cursor |
| `F1` | About |

### Sites and archives

| | |
| --- | --- |
| `Ctrl+F` | the saved-site manager |
| `Ctrl+N` | quick connect to one host |
| `cd ftp://…` | go there in the active pane |
| `Alt+F5` | pack the marked entries into an archive |
| `Alt+F6` | extract an archive into the other pane |
| `Alt+F9` | test an archive |

### Other

| | |
| --- | --- |
| `Shift+F7` `Alt+F7` | search |
| `Alt+F8` | command history |
| `Alt+C` | put `cd ` on the command line |
| `Shift+Esc` | minimize |
| `Alt+F4` | quit |

## Where things are

    src/main.cpp         window, SDL and ImGui setup, the frame loop, key dispatch
    src/panes.{h,cpp}    the whole file-manager surface: panes, tabs, dialogs, menus,
                         quick view, background transfers, the remote pane mode
    src/filesystem.*     local filesystem operations, trash, tree counting
    src/archive.*        browse, extract, pack — driven by the installed zip/tar/7z
    src/remote.*         FTP/FTPS/SFTP through libcurl: sites file, parsers, transport
    src/config.*         ~/.config/mc-clone/config, and the 0600 sites file beside it
    src/theme.*          eight themes and the accent colours the panes draw with
    tests/               the four suites and the FTP fixture
    scripts/get-deps.sh  the pinned Dear ImGui checkout
    FEATURES_PLAN.md     the project's own record: what landed, what was refused, and why

## Things worth knowing before you rely on it

- **Archives are shelled out to, not linked in.** The app drives `zip`, `unzip`,
  `tar` and `7z`. That buys format coverage without a new dependency and costs a
  real caveat: a pack password is visible in the process list while the tool runs,
  and the pack dialog says so in one line.
- **Saved site passwords are cleartext**, in `~/.config/mc-clone/ftp_sites`, created
  `0600`. The main config is written world-readable, which is why it stays
  password-free.
- **A site's directory listing is fetched in the frame that asks for it**, so a slow
  server freezes the UI until it answers. Transfers and quick-view previews run on
  worker threads; listings do not yet.
- **SFTP host keys are trust-on-first-use**, with the fingerprint remembered per
  site. It is the weakest part of the remote feature.
- **`FEATURES_PLAN.md` refers to `extern_dont_commit/`** — the Total Commander
  shortcut reference this was written against, and Midnight Commander's source.
  Both are read for reference and neither is shipped here. `scripts/get-deps.sh`
  brings the one thing actually needed to build.
- The state is honest about being unfinished: version `0.9.0`, and the plan names
  what is still open.

## License

MIT — see `LICENSE`.

[ci]: https://github.com/pgarba/immc/actions/workflows/ci.yml
