# immc

[![build and test]][ci]

A dual-pane file manager for the keyboard. Two file panes, a console window, and the
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

Dear ImGui is not vendored in this repository. Fetch the pinned copies first:

    scripts/get-deps.sh

That clones `ocornut/imgui` to the exact commit this project is built and tested
against (`v1.92.9b-112-gf502f9c98`) into `extern_dont_commit/imgui`, which is where
`CMakeLists.txt` looks for it, and downloads `stb_image.h` - the picture decoder
quick view uses - to `extern_dont_commit/stb`, at a pinned commit with its SHA-256
checked. The commit is pinned rather than "latest" because
ImGui's master carries a `WIP` version string and moves, so an unpinned clone can
break a build whose own source never changed.

Both are optional in the same way. A build without ImGui does not exist; a build
without `stb_image.h` configures with a message, runs, and previews a picture by
saying it has no decoder rather than by failing.

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

Eight suites, about 2,500 checks between them:

| suite | what it covers |
| --- | --- |
| `panes` | the pane logic: marking, selection, sorting, filtering, tabs, directory history, the operation dialogs and their keyboard ring, quick view including its picture path, background transfers, config round-trips through the real file. It `#include`s `src/panes.cpp` so the file-static helpers can be exercised directly. |
| `archive` | the archive backends, against real `zip`/`tar`/`7z` files that the test builds with the same tools the app shells out to |
| `remote` | the URL layer, the sites file and its `0600` mode, the MLSD and LIST dialects against recorded server output, and then the transport against a real FTP server the test starts on `127.0.0.1` (`tests/ftp_server.py`) |
| `sfv` | CRC-32 against the published check value, and the `.sfv` parser against the forms real files take: comments, names with spaces, checksum-first lines, backslash separators, broken UTF-8 |
| `img` | picture recognition and decoding: format sniffing from bytes, PNG and BMP fixtures built in the test, all eight EXIF orientations, malformed EXIF, and the size guard that refuses a 9000x9000 header before allocating it |
| `texture` | the GL texture upload, against a real context, including the way out when there is no context at all |
| `gl-preview` | the whole feature on screen: a fixture decoded, cached, textured, drawn by the real renderer, and read back out of the framebuffer as magenta pixels |
| `version` | `mc-clone --version`, which runs before SDL is initialised |

Three of them skip half their work rather than failing for want of an environment,
and say so on the way out: `remote` without libcurl or a startable fixture, `img` and
`panes` without the fetched decoder, `texture` and `gl-preview` without a display. A
check that cannot run announces itself. The no-libcurl configuration is verified as a
first-class build, not assumed:

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
| `F3` | quick view: a picture if the file is one, otherwise the beginning of the file under the cursor, including one on a site or inside an archive |
| `Ctrl+Q` | quick view as a pane, instead of a file pane |
| `Alt+F3` | open with the desktop's default program |
| `F4` | edit in the terminal editor |
| `F5` `F6` | copy, move — a dialog first, and `Tab` walks it |
| `Y` `N` `R` | when a target already exists: overwrite it, leave it alone, or write beside it (`report.pdf` → `report (2).pdf`). `Esc` cancels the operation instead |
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
| `Alt+Enter` | properties: size and counts, and the mode, owner and group — editable on a local file, mode only on a site |

Each pane ends with a status row: what the files on screen add up to, how many
files there are, and how many directories. With marks it becomes
`2/9 files  1/2 dirs  8.1/64.4M` — marked first — so the row answers *what am I
about to copy* without asking `Ctrl+L` first. The bytes are the files in that
folder: a directory contributes its count and no size, which is what `Ctrl+L` and
`Alt+Shift+Enter` exist for. The counts follow the quick filter, so they describe
the rows you can see, and in branch view they describe the subtree branch view
listed. Inside an archive the sizes are the members' sizes as the archive lists
them, and the row says `packed`.

There is no command line. `Ctrl+Enter` opens a console window over the panes: type
a command, `Enter` runs it in the directory the focused pane is showing, and its
output lands in the window's transcript. `Esc` hides the window and keeps the
transcript, so the console you opened in one directory is the same console in the
next one, and the same one after a restart. While the console holds the keyboard its
field owns it: `Space` types a space, `Ctrl+C` and `Ctrl+V` copy and paste inside
the line, and nothing marks a file behind your back. With the window closed the
marking keys belong to the panes unconditionally — the argument about whether an
empty box meant typing or marking is gone, because there is no box that is always
focused. `Enter` on a file is unchanged, and `Enter` on an executable runs it and
puts its output in the console.

### Console

| | |
| --- | --- |
| `Ctrl+Enter` | open the console; run the line when it is already open |
| `Enter` | run it |
| `Esc` | cancel a running command, then hide the window — the backlog stays |
| `Up` `Down` | recall commands |
| `Alt+F8` | the history list, inside the window |
| `Alt+PgUp` `Alt+PgDn` | walk the history |
| `Ctrl+P` | put the pane's path in the console |
| `Alt+C` | open it with `cd ` already typed |
| `cd` `pwd` `clear` `history` `exit` | builtins; `cd` moves the focused pane |

Anything else goes to `/bin/sh`, so pipes and redirection work. A command runs on a
worker thread: a `find` that takes a minute shows its results during the minute and
the panes keep answering underneath it.

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
| `Ctrl+D` | leave the site this pane is on, and close its connection (`Commands → Disconnect from site`) |
| `Commands → Verify SFV` | check the focused pane's directory against the `.sfv` in it |
| `cd ftp://…` | go there in the active pane |
| `Alt+F5` | pack the marked entries into an archive |
| `Alt+F6` | extract an archive into the other pane |
| `Alt+F9` | test an archive |

### Other

| | |
| --- | --- |
| `Shift+F7` `Alt+F7` | search |
| `Shift+Esc` | minimize |
| `Alt+F4` | quit |

## Transfers, resume and checksums

A copy or move that crosses a network runs on its own thread and shows as a row in
the strip under the panes. Aborting one leaves the row there with a **Resume** button:
the answer is Resume / Start over / Skip (`R / S / Esc`), and a resumed download picks
up from the `<name>.part` beside the target rather than from the beginning.

When a target already exists the question has three answers — **Overwrite**, **Skip**,
**Rename** (`Y / N / R`, `Esc` cancels the whole operation). Rename writes beside the
existing file: `name.ext` becomes `name (2).ext`, then `(3)`.

A download that lands in a folder holding a `.sfv` offers to verify against it:
**Verify / Not now / Always** (`V / N / A`). Verification runs on a thread, counts up
in the strip, and reports mismatched and missing files in a window. `Commands →
Verify SFV` runs the same check on the focused pane's directory with no fetching
involved, and the report's **Re-fetch the bad ones** rebuilds the failed names out of
the transfer that fetched them. The behaviour after a download is a line in the config
file:

    sfv_verify = ask      # ask | always | never

## Pictures in quick view

`F3` on a picture shows the picture. What a file *is* comes from its bytes and not
from its name, so `photo.JPG`, `notes.jpg.txt` and a PNG called `data` are all
previewed, and a format this build recognises but does not decode - webp, TIFF, SVG,
ICO - is named in words instead of being answered with a hex dump.

A picture is read and decoded on a worker, never in the frame that asked for it, and
the pixels are kept so the window can be resized without decoding again. A JPEG is
turned to match the way the camera held it before anything is drawn, and the caption
says both numbers: what the file declared and what is being shown.

Only files on this disk and members of an archive are previewed this way. A row on a
site keeps the text preview it always had, because a preview is not a reason to pull a
whole photograph down to find out what a row is.

Two lines in the config file:

    qv_images = 1         # 0 puts the hex dump back
    quickview_w = 720     # the window's size, as before

## Where things are

    src/main.cpp         window, SDL and ImGui setup, the frame loop, key dispatch
    src/panes.{h,cpp}    the whole file-manager surface: panes, tabs, dialogs, menus,
                         quick view, background transfers, the remote pane mode
    src/filesystem.*     local filesystem operations, trash, tree counting
    src/archive.*        browse, extract, pack — driven by the installed zip/tar/7z
    src/proc.*           the child-process runner the archives and the console share
    src/remote.*         FTP/FTPS/SFTP through libcurl: sites file, parsers, transport
    src/sfv.*            CRC-32 and the .sfv parser, with no dependency on zlib
    src/img.*            picture recognition, decoding, EXIF orientation, downscaling
    src/texture.*        decoded pixels to a GL texture, and back out again
    src/config.*         ~/.config/mc-clone/config, and the 0600 sites file beside it
    src/theme.*          eight themes and the accent colours the panes draw with
    tests/               the eight suites, the FTP fixture, and the pictures the
                         picture tests build for themselves
    scripts/get-deps.sh  the pinned Dear ImGui checkout and the pinned stb_image.h
    FEATURES_PLAN.md     the project's own record: what landed, what was refused, and why

## Things worth knowing before you rely on it

- **Archives are shelled out to, not linked in.** The app drives `zip`, `unzip`,
  `tar` and `7z`. That buys format coverage without a new dependency and costs a
  real caveat: a pack password is visible in the process list while the tool runs,
  and the pack dialog says so in one line.
- **Saved site passwords are cleartext**, in `~/.config/mc-clone/ftp_sites`, created
  `0600`. The main config is written world-readable, which is why it stays
  password-free.
- **The console backlog is written to that world-readable config too**, because the
  transcript is the point of the feature. A command that carried a password is in
  the file; `clear` before quitting, or `console_save = 0`, keeps it out.
- **The console has no terminal to give a command.** stdout and stderr come back,
  stdin is `/dev/null`, so a tool that wants a password or a TTY (`vim`, `ssh`,
  `less`) fails immediately instead of waiting for one. `F4` is the answer for
  editors: it forks a terminal emulator so the editor gets a real TTY.
- **Resume is only as good as the server.** A download keeps its bytes in
  `<name>.part` and continues from there when the server honours `REST`; an upload
  continues with `APPE`. If the finished length does not match the file on the
  server, the partial is thrown away and the transfer is reported as needing to start
  again rather than accepted. Partial files survive quitting the app; the transfer
  rows do not.
- **A checksum that matches is not a proof.** `.sfv` carries CRC-32, which catches a
  truncated or damaged file. It is not a signature: a server that lies about where a
  resumed transfer starts can produce a file of the right length with the wrong bytes,
  and a length check cannot see that. `.md5` files beside a download are a different
  format with a different algorithm and are not verified here.
- **A preview is a look, not an import.** A picture is read up to 32 MB and refused
  above it, and a header claiming 9000x9000 is refused before any pixels are
  allocated. Decoded pictures are cached four at a time and 96 MB at a time, oldest
  first, because a decoded 4032x3024 photograph is 48 MB of pixels. What is shown is
  downscaled to 2048 on the longest side, so a preview of a large picture is a
  downscaled picture and the caption says so.
- **Ownership changes need root**, unless you are giving a file back to yourself. The
  Properties window reports `not permitted (ownership changes need root)` rather than
  reporting a change that did not happen. It uses `lchown`, so the link is chowned and
  not whatever it points at; on Linux a symlink has no mode of its own to set, and the
  window says so instead of silently chmod-ing the target.
- **FTP has no owner or group.** `SITE CHMOD` is a site extension rather than part of
  the protocol, so a server can refuse it; the Properties window says both instead of
  hiding the fields.
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
