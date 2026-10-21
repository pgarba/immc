#ifndef MC_TERMINAL_H
#define MC_TERMINAL_H

#include <string>
#include <vector>

// Finding something to run an editor inside.
//
// The file manager draws its own window and has no terminal to hand a TUI editor, so
// F4 has to launch a terminal emulator and put the editor inside it. Two things make
// that harder than it looks. There is no agreed name for a terminal - a machine may
// have any of a dozen, and this one has alacritty and none of the five names the app
// used to know. And there is no agreed way to pass a command to one: xterm and konsole
// take `-e prog args`, xfce4-terminal wants one quoted string it splits itself,
// gnome-terminal refuses a command without `--` in front of it, foot wants `--`, and
// wezterm wants `start --`. Getting the shape wrong produces a terminal window that
// opens, says nothing, and closes.
//
// So the shape is data, and the name is overridable: `terminal =` in the config, then
// $TERMINAL, then the list. A value in the config may carry its own flags
// ("terminal = gnome-terminal --"), which is what makes an unusual terminal usable
// without a code change.

struct TermLaunch {
    std::string name;                    // what was chosen, for the status line
    std::string note;                    // only on failure: what was looked for
    std::vector<std::string> argv;       // the command to run, editor and path last
};

// The editor's name: $EDITOR, or vim when that is unset. One place so the status line
// and the launch cannot disagree about what is being run.
const char* term_editor_name();

// The command line for one named terminal. Known names get their real shape; anything
// else - a terminal named in the config that is not in the table - gets `-e editor path`,
// which is the convention most of the rest follow. Exposed separately because the shape
// is the part worth checking without a display.
std::vector<std::string> term_launch_argv(const char* name, const char* editor,
                                          const std::string& path);

// Is this program on PATH (or a path the config named directly)? No shell is spawned:
// a fork per candidate, on a keypress, is how a lookup becomes a source of surprises.
bool term_which(const std::string& prog, std::string* resolved);

// The whole search. `preferred` is the config's `terminal =` value, which may be empty;
// `path` is the file the editor will be given. Order: the config value, then $TERMINAL,
// then the table. A config value that names a program which is not there is reported
// rather than quietly replaced by another terminal - the person who wrote the line chose
// that terminal on purpose.
TermLaunch term_find_launch(const char* preferred, const std::string& path);

// The names in the table, in the order they are probed. For the message that says what
// was looked for, and for the tests that check the order.
const std::vector<std::string>& term_candidates();

#endif
