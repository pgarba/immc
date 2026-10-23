#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>   // size_t, for mc_config_path
#include <string>
#include <vector>

// User configuration, stored as key = value lines in
// ~/.config/mc-clone/config (written back on exit).
static const int MC_MAX_TABS = 8;   // keep in sync with MAX_TABS in panes.h
// How much of the backlog the file carries. The panes keep more in memory; this
// is the part that survives a restart, and it is what a person reading the file
// has to skip past.
static const int MC_MAX_CONSOLE_ENTRIES = 50;   // keep in sync with panes.h

// One console command carried across a restart: the line, the directory it ran
// in, what it printed, and how it ended. Its own type rather than AppPanes'
// ConsoleEntry so reading the config does not require knowing what the panes
// look like.
struct MCConsoleEntry {
    std::string cwd;
    std::string cmd;
    std::string out;
    int  code = -1;
    bool builtin = false;
};

struct MCConfig {
    int   win_x, win_y;   // window position; -1 = let the window manager place it
    int   win_w, win_h;   // window size
    // Size of the F3 quick view window, in pixels: restored at startup and
    // written back on exit. ImGui's own window .ini is switched off, so this is
    // the only reason the preview is the size you left it. The position is not
    // saved - see the comment on qv_w in panes.h.
    int   qv_w, qv_h;
    char  font[1024];     // path to a .ttf/.otf font; empty = built-in font
    float font_size;      // font size in px; 0 = built-in default
    char  hinting[32];    // how glyphs meet the pixel grid: default | none | light |
                          // mono | "mono (no smoothing)". Names, not numbers, so a
                          // hand-edited config says what the dialog shows, and a
                          // number cannot mean something different next release.
    char  theme[64];      // dark | light | classic | gruvbox | gruvbox-light |
                          // solarized-dark | solarized-light

    // Quick filter preset applied with Ctrl+F12 (e.g. "*.cpp"); empty = none.
    char  filter_preset[256];

    // Listing options, restored at startup and written back on exit.
    int   view_mode;      // 0 = mid, 1 = brief, 2 = full (AppView in panes.h)
    int   sort_mode;      // FsSortMode in filesystem.h
    int   show_hidden;    // 1 = list dotfiles
    int   show_icons;     // 1 = a small icon in front of each name, as in TC
    int   trash;          // 1 = Del/F8 goes to the trash, Shift always direct

    // Column set of the long view (Shift+F1). Comma-separated; "name" is always
    // first. Vocabulary: name, ext, size, date, perms, type.
    char  columns[256];

    // Pane tabs (left/right), saved on exit and restored at startup.
    // count = 0 means "use the app defaults".
    char left_tabs[MC_MAX_TABS][4096];
    int  left_tab_count;
    int  left_tab_active;
    char right_tabs[MC_MAX_TABS][4096];
    int  right_tab_count;
    int  right_tab_active;

    // Console window size in pixels, restored at startup and written back on
    // exit, exactly like the quick view's.
    int  console_w, console_h;
    // 1 = the console backlog is written to this file on exit and read back at
    // startup. The transcript is plain text in a file this app writes with the
    // default mode, so a command that carried a password lands in it; the switch
    // exists so that can be turned off. It is a line in this file, not a dialog
    // setting: what it protects has to be switchable without opening a window.
    int  console_save;
    // The backlog itself, oldest first, capped by the panes module.
    std::vector<MCConsoleEntry> console_entries;

    // "ask" (default), "always" or "never": what happens after a download when the
    // folder it landed in holds a .sfv. A value this file does not carry, or carries
    // misspelled, is read as "ask": the default that asks is safer than the one that
    // stays quiet, and a typo in a config line should not silently turn off a check.
    char sfv_verify[16];

    // How the quick view reads a file that is not UTF-8: "auto" (default) transcodes
    // CP437 when the bytes look like ANSI art, "utf8" never transcodes, "cp437" always
    // does. An unrecognised word, like a typo, is read as "auto".
    char preview_codepage[16];

    // The terminal F4 runs the editor inside. Empty (default) means the built-in list
    // of names. It may name any program, and may carry that program's own flags:
    // "terminal = gnome-terminal --". $TERMINAL is honoured when this is empty.
    char terminal[128];

    // 1 (default): quick view draws a picture as a picture. 0: a picture row gets the
    // hex dump it always got. Here rather than in the Options dialog for the same
    // reason console_save is: it is a switch for the person who wants the old
    // behaviour back, and every dialog row is something everyone reads past.
    int qv_images;
};

// Absolute path of the config file, for the About window to name. Does not
// create anything.
void mc_config_path(char* out, size_t n);

void mc_config_default(MCConfig* c);
void mc_config_load(MCConfig* c);
void mc_config_save(const MCConfig* c);

#endif
