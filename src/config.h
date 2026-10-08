#ifndef CONFIG_H
#define CONFIG_H

// User configuration, stored as key = value lines in
// ~/.config/mc-clone/config (written back on exit).
static const int MC_MAX_TABS = 8;   // keep in sync with MAX_TABS in panes.h

struct MCConfig {
    int   win_x, win_y;   // window position; -1 = let the window manager place it
    int   win_w, win_h;   // window size
    char  font[1024];     // path to a .ttf/.otf font; empty = built-in font
    float font_size;      // font size in px; 0 = built-in default
    char  theme[64];      // dark | light | classic | gruvbox | gruvbox-light |
                          // solarized-dark | solarized-light

    // Quick filter preset applied with Ctrl+F12 (e.g. "*.cpp"); empty = none.
    char  filter_preset[256];

    // Listing options, restored at startup and written back on exit.
    int   view_mode;      // 0 = mid, 1 = brief, 2 = full (AppView in panes.h)
    int   sort_mode;      // FsSortMode in filesystem.h
    int   show_hidden;    // 1 = list dotfiles
    int   trash;          // 1 = Del/F8 goes to the trash, Shift always direct

    // Pane tabs (left/right), saved on exit and restored at startup.
    // count = 0 means "use the app defaults".
    char left_tabs[MC_MAX_TABS][4096];
    int  left_tab_count;
    int  left_tab_active;
    char right_tabs[MC_MAX_TABS][4096];
    int  right_tab_count;
    int  right_tab_active;
};

void mc_config_default(MCConfig* c);
void mc_config_load(MCConfig* c);
void mc_config_save(const MCConfig* c);

#endif
