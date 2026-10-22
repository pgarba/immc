#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "platform.h"

static std::string config_dir() {
    return plat_config_dir();
}

static std::string config_path() {
    return config_dir() + "/config";
}

void mc_config_path(char* out, size_t n) {
    snprintf(out, n, "%s", config_path().c_str());
}

static void ensure_config_dir() {
    plat_make_config_dir();
}

void mc_config_default(MCConfig* c) {
    c->win_x = -1; c->win_y = -1;
    c->win_w = 1280; c->win_h = 800;
    // Keep in sync with qv_w/qv_h in AppPanes (panes.h): a config that says
    // nothing and an app that was never told have to agree on the same window.
    c->qv_w = 720; c->qv_h = 460;
    // Keep in sync with console_w/console_h in AppPanes (panes.h).
    c->console_w = 760; c->console_h = 420;
    c->console_save = 1;
    snprintf(c->sfv_verify, sizeof(c->sfv_verify), "ask");
    snprintf(c->preview_codepage, sizeof(c->preview_codepage), "auto");
    c->terminal[0] = '\0';
    c->qv_images = 1;
    c->console_entries.clear();
    c->font[0] = '\0';
    c->font_size = 0.0f;
    snprintf(c->hinting, sizeof(c->hinting), "default");
    snprintf(c->theme, sizeof(c->theme), "dark");
    c->filter_preset[0] = '\0';
    c->view_mode = 0;      // VIEW_MID
    c->sort_mode = 0;      // FS_SORT_NAME
    c->show_hidden = 0;
    c->trash = 1;          // Del/F8 goes to the trash when a helper exists
    snprintf(c->columns, sizeof(c->columns), "name,size,date,perms");
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

// A console transcript line is one config line, and a command's output is
// whatever the tool printed. So the characters this file format gives meaning to
// are escaped on the way out and put back on the way in: a newline would end the
// value and leave the rest of the output to be parsed as keys, and a bare
// backslash would change what the next escape means.
static std::string cfg_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:   o += ch;     break;
        }
    }
    return o;
}

static std::string cfg_unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        switch (s[++i]) {
        case 'n':  o += '\n'; break;
        case 'r':  o += '\r'; break;
        case 't':  o += '\t'; break;
        case '\\': o += '\\'; break;
        default:   o += s[i]; break;   // an unknown escape is the character itself
        }
    }
    return o;
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
        else if (!strcmp(key, "quickview_w")) c->qv_w = atoi(val);
        else if (!strcmp(key, "quickview_h")) c->qv_h = atoi(val);
        else if (!strcmp(key, "console_w"))     c->console_w = atoi(val);
        else if (!strcmp(key, "console_h"))     c->console_h = atoi(val);
        else if (!strcmp(key, "console_save"))  c->console_save = atoi(val);
        else if (!strcmp(key, "sfv_verify"))    snprintf(c->sfv_verify, sizeof(c->sfv_verify), "%s", val);
        else if (!strcmp(key, "preview_codepage"))
            snprintf(c->preview_codepage, sizeof(c->preview_codepage), "%s", val);
        else if (!strcmp(key, "terminal"))
            snprintf(c->terminal, sizeof(c->terminal), "%s", val);
        else if (!strcmp(key, "qv_images"))     c->qv_images = atoi(val);
        else if (!strcmp(key, "font"))      snprintf(c->font, sizeof(c->font), "%s", val);
        else if (!strcmp(key, "font_size")) c->font_size = (float)atof(val);
        else if (!strcmp(key, "font_hinting")) snprintf(c->hinting, sizeof(c->hinting), "%s", val);
        else if (!strcmp(key, "theme"))     snprintf(c->theme, sizeof(c->theme), "%s", val);
        else if (!strcmp(key, "filter_preset")) snprintf(c->filter_preset, sizeof(c->filter_preset), "%s", val);
        else if (!strcmp(key, "view_mode"))   c->view_mode = atoi(val);
        else if (!strcmp(key, "sort_mode"))   c->sort_mode = atoi(val);
        else if (!strcmp(key, "show_hidden")) c->show_hidden = atoi(val);
        else if (!strcmp(key, "trash"))       c->trash = atoi(val);
        else if (!strcmp(key, "columns"))     snprintf(c->columns, sizeof(c->columns), "%s", val);
        else if (!strcmp(key, "left_tab_active"))  c->left_tab_active = atoi(val);
        else if (!strcmp(key, "right_tab_active")) c->right_tab_active = atoi(val);
        else {
            // console<N>_cwd / _cmd / _out / _code / _builtin: one command of the
            // backlog. The index is read out of the key, so a hand-edited file
            // with a gap in it still lands in the right slot instead of shifting
            // everything up one command.
            int idx = -1;
            char field[16];
            if (sscanf(key, "console%d_%15s", &idx, field) == 2 && idx >= 0 &&
                idx < MC_MAX_CONSOLE_ENTRIES) {
                if ((int)c->console_entries.size() <= idx) c->console_entries.resize(idx + 1);
                MCConsoleEntry& e = c->console_entries[idx];
                if      (!strcmp(field, "cwd"))     e.cwd = cfg_unescape(val);
                else if (!strcmp(field, "cmd"))     e.cmd = cfg_unescape(val);
                else if (!strcmp(field, "out"))     e.out = cfg_unescape(val);
                else if (!strcmp(field, "code"))    e.code = atoi(val);
                else if (!strcmp(field, "builtin")) e.builtin = atoi(val) != 0;
            }

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
    // A quick view smaller than this has no room for the two header lines and a
    // row of text, and a window that small is hard to drag back by its title bar.
    if (c->qv_w < 200) c->qv_w = 720;
    if (c->qv_h < 120) c->qv_h = 460;
    if (c->console_w < 240) c->console_w = 760;
    if (c->console_h < 120) c->console_h = 420;
    c->console_save = c->console_save ? 1 : 0;
    c->qv_images = c->qv_images ? 1 : 0;
    // Only the three words mean anything. Anything else, including a typo, falls
    // back to asking.
    if (strcmp(c->sfv_verify, "always") && strcmp(c->sfv_verify, "never"))
        snprintf(c->sfv_verify, sizeof(c->sfv_verify), "ask");
    // The same rule as sfv_verify: three words mean anything, and a typo falls back to
    // the one that guesses from the bytes rather than to a silent override.
    if (strcmp(c->preview_codepage, "utf8") && strcmp(c->preview_codepage, "cp437"))
        snprintf(c->preview_codepage, sizeof(c->preview_codepage), "auto");
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
        "# How glyphs meet the pixel grid. One of:\n"
        "#   default                 the font's own hinter, normal strength\n"
        "#   none                    no hinting (what stb_truetype always does)\n"
        "#   light                   snapped vertically only, keeps glyph spacing\n"
        "#   mono                    snapped both ways, crispest small monospace\n"
        "#   mono (no smoothing)     as mono with no anti-aliasing\n"
        "# Needs the FreeType loader; a build without it ignores this line.\n"
        "font_hinting = %s\n"
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
        "trash = %d\n"
        "\n# Columns of the long view (Shift+F1); name is always first\n"
        "columns = %s\n"
        "\n# Size in pixels of the F3 quick view window (written on exit).\n"
        "# The window opens here next time; its position is left to the screen.\n"
        "quickview_w = %d\n"
        "quickview_h = %d\n",
        c->win_x, c->win_y, c->win_w, c->win_h,
        c->font, (double)c->font_size, c->hinting, c->theme,
        c->filter_preset, c->view_mode, c->sort_mode, c->show_hidden, c->trash,
        c->columns, c->qv_w, c->qv_h);

    fprintf(f,
        "\n# Pane tabs restored at startup (written on exit)\n"
        "# left_tab_active/right_tab_active pick the focused tab.\n");
    for (int i = 0; i < c->left_tab_count; i++)
        fprintf(f, "left_tab%d = %s\n", i, c->left_tabs[i]);
    fprintf(f, "left_tab_active = %d\n", c->left_tab_active);
    for (int i = 0; i < c->right_tab_count; i++)
        fprintf(f, "right_tab%d = %s\n", i, c->right_tabs[i]);
    fprintf(f, "right_tab_active = %d\n", c->right_tab_active);

    // The console backlog: the commands and their output, oldest first, so the
    // transcript you hid with Esc is still there in the next session and in a
    // directory you have never opened before. Only the tail is kept, and only a
    // bounded amount of each command's output, because this file is meant to stay
    // readable by the person it belongs to.
    fprintf(f,
        "\n# Console window size in pixels (written on exit)\n"
        "console_w = %d\n"
        "console_h = %d\n"
        "\n# 1 = keep the console backlog in this file across sessions. The\n"
        "# transcript is plain text: a command that carried a password is written\n"
        "# here too, which is what this switch is for. 0 = start empty.\n"
        "console_save = %d\n"
        // What a download does when the folder it landed in holds a .sfv: "ask"
        // offers the check, "always" runs it without asking, "never" leaves it
        // alone. A verification is also always available from the Commands menu.
        "\n# After a download that landed beside a .sfv: ask | always | never\n"
        "sfv_verify = %s\n"
        // A file from the DOS world writes its box drawing as single bytes (CP437).
        // "auto" reads those as CP437 when the bytes look like a drawing, "utf8" leaves
        // every file alone, "cp437" forces it for the drawing the guess missed. A font
        // without box-drawing glyphs shows a line about it in the quick view.
        "\n# How the quick view reads a file that is not UTF-8: auto | utf8 | cp437\n"
        "preview_codepage = %s\n"
        // F4 runs the editor inside a terminal emulator, because a TUI editor needs a
        // TTY this window cannot give it. Empty means the built-in list of terminals;
        // a name here can be any terminal, with the flags that one needs written out.
        "\n# Terminal for the editor (F4). Empty = the built-in list. $TERMINAL also works\n"
        "terminal = %s\n"
        "qv_images = %d\n",
        c->console_w, c->console_h, c->console_save, c->sfv_verify,
        c->preview_codepage, c->terminal, c->qv_images);

    if (c->console_save) {
        fprintf(f,
            "\n# Console backlog, oldest first. A command's output is escaped so that\n"
            "# one entry stays one line: newline, tab and carriage return come out as\n"
            "# backslash-n, backslash-t and backslash-r, and a backslash is doubled.\n");
        int n = (int)c->console_entries.size();
        if (n > MC_MAX_CONSOLE_ENTRIES) n = MC_MAX_CONSOLE_ENTRIES;
        for (int i = 0; i < n; i++) {
            const MCConsoleEntry& e = c->console_entries[i];
            fprintf(f, "console%d_cwd = %s\n", i, cfg_escape(e.cwd).c_str());
            fprintf(f, "console%d_cmd = %s\n", i, cfg_escape(e.cmd).c_str());
            fprintf(f, "console%d_out = %s\n", i, cfg_escape(e.out).c_str());
            fprintf(f, "console%d_code = %d\n", i, e.code);
            fprintf(f, "console%d_builtin = %d\n", i, e.builtin ? 1 : 0);
        }
    }

    fclose(f);
}
