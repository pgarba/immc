#ifndef THEME_H
#define THEME_H

#include "imgui.h"

// Accent colors the file panels use, set by the active theme.
struct MCThemeColors {
    ImVec4 title_focused;    // focused pane path bar
    ImVec4 title_focused_hov;
    ImVec4 title_unfocused;  // unfocused pane path bar
    ImVec4 dir;              // directory name text
    ImVec4 exec;             // executable name text
    ImVec4 diff_missing;     // Shift+F2 compare: absent from the other pane
    ImVec4 diff_differ;      // Shift+F2 compare: present but size/type differs
    ImU32  marked_bg;        // marked row background
    ImU32  cursor_bg;        // cursor row background
    ImU32  cursor_outline;   // outline when marked + cursor coincide
    ImVec4 window_bg;        // GL clear color (matches ImGuiCol_WindowBg)
};

// Applies the named theme's ImGui style and accent colors.
// Names: dark | light | classic | gruvbox | gruvbox-light |
//        solarized-dark | solarized-light
void mc_apply_theme(const char* name);

// Accent colors of the currently active theme.
const MCThemeColors* mc_theme_colors();

#endif
