#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

static std::string config_dir() {
    const char* home = getenv("HOME");
    if (!home || !*home) return ".mc-clone";
    return std::string(home) + "/.config/mc-clone";
}

static std::string config_path() {
    return config_dir() + "/config";
}

static void ensure_config_dir() {
    std::string dir = config_dir();
    // Create ~/.config and ~/.config/mc-clone (ignore "already exists").
    std::string parent = dir.substr(0, dir.find_last_of('/'));
    mkdir(parent.c_str(), 0755);
    mkdir(dir.c_str(), 0755);
}

void mc_config_default(MCConfig* c) {
    c->win_x = -1; c->win_y = -1;
    c->win_w = 1280; c->win_h = 800;
    c->font[0] = '\0';
    c->font_size = 0.0f;
    snprintf(c->theme, sizeof(c->theme), "dark");
    c->filter_preset[0] = '\0';
    c->view_mode = 0;      // VIEW_MID
    c->sort_mode = 0;      // FS_SORT_NAME
    c->show_hidden = 0;
    c->trash = 1;          // Del/F8 goes to the trash when a helper exists
    c->left_tab_count = 0;  c->left_tab_active = 0;
    c->right_tab_count = 0; c->right_tab_active = 0;
    for (int i = 0; i < MC_MAX_TABS; i++) {
        c->left_tabs[i][0] = '\0';
        c->right_tabs[i][0] = '\0';
    }
}

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

void mc_config_load(MCConfig* c) {
    mc_config_default(c);
    FILE* f = fopen(config_path().c_str(), "r");
    if (!f) return;

    char line[4300];
    while (fgets(line, sizeof(line), f)) {
        char* s = trim(line);
        if (*s == '\0' || *s == '#') continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trim(s);
        char* val = trim(eq + 1);

        if      (!strcmp(key, "window_x"))  c->win_x = atoi(val);
        else if (!strcmp(key, "window_y"))  c->win_y = atoi(val);
        else if (!strcmp(key, "window_w"))  c->win_w = atoi(val);
        else if (!strcmp(key, "window_h"))  c->win_h = atoi(val);
        else if (!strcmp(key, "font"))      snprintf(c->font, sizeof(c->font), "%s", val);
        else if (!strcmp(key, "font_size")) c->font_size = (float)atof(val);
        else if (!strcmp(key, "theme"))     snprintf(c->theme, sizeof(c->theme), "%s", val);
        else if (!strcmp(key, "filter_preset")) snprintf(c->filter_preset, sizeof(c->filter_preset), "%s", val);
        else if (!strcmp(key, "view_mode"))   c->view_mode = atoi(val);
        else if (!strcmp(key, "sort_mode"))   c->sort_mode = atoi(val);
        else if (!strcmp(key, "show_hidden")) c->show_hidden = atoi(val);
        else if (!strcmp(key, "trash"))       c->trash = atoi(val);
        else if (!strcmp(key, "left_tab_active"))  c->left_tab_active = atoi(val);
        else if (!strcmp(key, "right_tab_active")) c->right_tab_active = atoi(val);
        else {
            for (int i = 0; i < MC_MAX_TABS; i++) {
                char k[24];
                snprintf(k, sizeof(k), "left_tab%d", i);
                if (!strcmp(key, k)) {
                    snprintf(c->left_tabs[i], sizeof(c->left_tabs[i]), "%s", val);
                    if (i + 1 > c->left_tab_count) c->left_tab_count = i + 1;
                    break;
                }
                snprintf(k, sizeof(k), "right_tab%d", i);
                if (!strcmp(key, k)) {
                    snprintf(c->right_tabs[i], sizeof(c->right_tabs[i]), "%s", val);
                    if (i + 1 > c->right_tab_count) c->right_tab_count = i + 1;
                    break;
                }
            }
        }
    }
    fclose(f);

    if (c->win_w < 200) c->win_w = 1280;
    if (c->win_h < 200) c->win_h = 800;
    if (c->left_tab_count  < 0) c->left_tab_count  = 0;
    if (c->left_tab_count  > MC_MAX_TABS) c->left_tab_count = MC_MAX_TABS;
    if (c->right_tab_count < 0) c->right_tab_count = 0;
    if (c->right_tab_count > MC_MAX_TABS) c->right_tab_count = MC_MAX_TABS;
    if (c->left_tab_active  < 0 || c->left_tab_active  >= c->left_tab_count)  c->left_tab_active  = 0;
    if (c->right_tab_active < 0 || c->right_tab_active >= c->right_tab_count) c->right_tab_active = 0;
    if (c->view_mode < 0 || c->view_mode > 2) c->view_mode = 0;
    if (c->sort_mode < 0 || c->sort_mode > 4) c->sort_mode = 0;
    c->show_hidden = c->show_hidden ? 1 : 0;
    c->trash = c->trash ? 1 : 0;
}

void mc_config_save(const MCConfig* c) {
    ensure_config_dir();
    FILE* f = fopen(config_path().c_str(), "w");
    if (!f) return;

    fprintf(f,
        "# mc-clone configuration\n"
        "\n"
        "# Window position on screen (-1 = let the window manager decide)\n"
        "window_x = %d\n"
        "window_y = %d\n"
        "\n"
        "# Window size in pixels\n"
        "window_w = %d\n"
        "window_h = %d\n"
        "\n"
        "# Font: path to a .ttf/.otf file, empty for the built-in pixel font\n"
        "# Examples: /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf\n"
        "font = %s\n"
        "\n"
        "# Font size in px (0 = built-in default)\n"
        "font_size = %g\n"
        "\n"
        "# Color theme: dark | light | classic | gruvbox | gruvbox-light |\n"
        "#              solarized-dark | solarized-light\n"
        "theme = %s\n"
        "\n"
        "# Quick filter preset for Ctrl+F12 (empty = none), e.g. *.cpp\n"
        "filter_preset = %s\n"
        "\n"
        "# Listing, restored at startup: view_mode 0=mid 1=brief 2=full,\n"
        "# sort_mode 0=name 1=ext 2=size 3=date 4=unsorted, show_hidden 1=dotfiles\n"
        "# trash 1 = Del/F8 goes to the desktop trash (Shift+Del is always direct)\n"
        "view_mode = %d\n"
        "sort_mode = %d\n"
        "show_hidden = %d\n"
        "trash = %d\n",
        c->win_x, c->win_y, c->win_w, c->win_h,
        c->font, (double)c->font_size, c->theme,
        c->filter_preset, c->view_mode, c->sort_mode, c->show_hidden, c->trash);

    fprintf(f,
        "\n# Pane tabs restored at startup (written on exit)\n"
        "# left_tab_active/right_tab_active pick the focused tab.\n");
    for (int i = 0; i < c->left_tab_count; i++)
        fprintf(f, "left_tab%d = %s\n", i, c->left_tabs[i]);
    fprintf(f, "left_tab_active = %d\n", c->left_tab_active);
    for (int i = 0; i < c->right_tab_count; i++)
        fprintf(f, "right_tab%d = %s\n", i, c->right_tabs[i]);
    fprintf(f, "right_tab_active = %d\n", c->right_tab_active);

    fclose(f);
}
