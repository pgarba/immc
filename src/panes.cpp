#include "panes.h"
#include "filesystem.h"
#include "theme.h"
#include "config.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>   // ClearActiveID: drop the filter field's keyboard focus
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include <SDL3/SDL_opengl.h>
#include <string.h>
#include <strings.h>   // strncasecmp: quick search prefix match
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <libgen.h>
#include <float.h>
#include <time.h>
#include <algorithm>
#include <map>

// Rows per Page Up / Page Down step (approx. visible rows in a 800px pane)
static const int PAGE_SIZE = 18;

AppPanes::AppPanes() {
    left.entries = nullptr;
    left.entry_count = 0;
    left.selected = 0;
    left.anchor = 0;
    left.scroll_request = false;
    right.entries = nullptr;
    right.entry_count = 0;
    right.selected = 0;
    right.anchor = 0;
    right.scroll_request = false;
    left.path_edit_active = false;
    right.path_edit_active = false;
    left.path_focus_request = false;
    right.path_focus_request = false;
    left.quick_view = false;
    right.quick_view = false;
    left.cursor_screen_x = 0.0f; left.cursor_screen_y = 0.0f;
    right.cursor_screen_x = 0.0f; right.cursor_screen_y = 0.0f;
    left.list_gen = 0; right.list_gen = 0;
    left.list_mode = LIST_NORMAL; right.list_mode = LIST_NORMAL;
    left.page_size = PAGE_SIZE;
    right.page_size = PAGE_SIZE;
    left.history_count = 0; left.history_pos = 0;
    right.history_count = 0; right.history_pos = 0;
    left.sel_mem_count = 0;
    right.sel_mem_count = 0;
    left.tab_count = 0; left.tab_active = 0;
    right.tab_count = 0; right.tab_active = 0;
    left.filter[0] = '\0'; left.last_filter[0] = '\0'; left.filter_applied[0] = '\0';
    left.filter_active = false; left.filter_field_active = false; left.filter_focus_request = false;
    left.filter_exec_only = false;
    left.quick[0] = '\0'; left.quick_active = false;
    right.filter[0] = '\0'; right.last_filter[0] = '\0'; right.filter_applied[0] = '\0';
    right.filter_active = false; right.filter_field_active = false; right.filter_focus_request = false;
    right.filter_exec_only = false;
    right.quick[0] = '\0'; right.quick_active = false;
    left_focus = true;
    active_path_pane = -1;
    op = OP_NONE;
    op_name[0] = '\0';
    op_input_active = false;
    status_msg[0] = '\0';
    cmd[0] = '\0';
    cmd_hist_count = 0;
    cmd_hist_pos = 0;
    output_open = false;
    output_text[0] = '\0';
    confirm = CONF_NONE;
    confirm_text[0] = '\0';
    hotlist_open = false;
    dirhist_open = false;
    cmdhist_open = false;
    volumes_open = false;
    volumes_target = 0;
    search_open = false;
    search_pattern[0] = '\0';
    search_done = false;
    search_input_active = false;
    viewer_open = false;
    viewer_path[0] = '\0';
    props_open = false;
    props_path[0] = '\0';
    props_size = 0;
    props_files = 0;
    props_dirs = 0;
    about_open = false;
    menu_open = false; menu_alive = false;
    pane_menu_open = false; pane_menu_alive = false;
    entry_menu_open = false; ctx_menu_alive = false;
    compare_on = false;
    columns_open = false;
    snprintf(columns, sizeof(columns), "name,size,date,perms");
    trash_mode = 1;
    delete_trash = 0;
    tree_open = false;
    tree_target = 0;
    quick_view_pane = false;
    popup_close_request = false;
    prog_active = false;
    prog_is_move = false;
    prog_is_delete = false;
    prog_src[0] = '\0';
    prog_dst[0] = '\0';
    prog_done = 0;
    prog_total = 0;
    prog_files_done = 0;
    prog_files_total = 0;
    prog_t0 = 0;
    prog_last_pump = 0;
    pending_op = 0;
    pending_confirm_yes = false;
    sdl_window = nullptr;
    view_mode = VIEW_MID;
    show_hidden = false;
    filter_preset[0] = '\0';
    quit_request = false;
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

static int marked_count(const Pane& p) {
    int n = 0;
    for (size_t i = 0; i < p.marked.size(); i++)
        if (p.marked[i]) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Quick filter (TC Schnellfilter)
// ---------------------------------------------------------------------------
//
// `view` maps visible rows onto indices in `entries`. marks, selected and the
// per-directory selection memory are all index-coupled to `entries`, so the
// filter must never reorder or shrink `entries` - only `view` changes.

// Case-insensitive glob: '*' matches any run of characters, '?' any single one.
static bool glob_match(const char* pat, const char* s) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return true;
            for (; *s; s++) if (glob_match(pat, s)) return true;
            return false;
        }
        if (!*s) return false;
        if (*pat == '?') { pat++; s++; continue; }   // any single character
        if (tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return false;
        pat++; s++;
    }
    return *s == '\0';
}

// Plain text matches as a case-insensitive substring; a pattern containing
// '*' or '?' is glob-matched against the whole name, like TC.
// Path of a row inside its pane: the full relative path in branch view, the
// plain name otherwise. Every path and every label built from a listing goes
// through this.
static const char* entry_rel(const Pane& p, int idx) {
    if (p.list_mode == LIST_BRANCH && idx >= 0 && idx < (int)p.rel.size())
        return p.rel[idx].c_str();
    return p.entries[idx]->name;
}

static bool filter_matches(const char* name, const char* pat) {
    if (!pat[0]) return true;
    if (strpbrk(pat, "*?")) return glob_match(pat, name);
    for (const char* h = name; *h; h++) {
        const char* a = h; const char* b = pat;
        while (*b && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static void rebuild_view(Pane& p) {
    p.view.clear();
    p.view.reserve(p.entry_count);
    for (int i = 0; i < p.entry_count; i++) {
        // ".." stays visible so the pane can always be walked out of.
        if (strcmp(p.entries[i]->name, "..") == 0) { p.view.push_back(i); continue; }
        if (p.filter_exec_only &&
            !is_executable(p.entries[i]->name, p.entries[i]->mode)) continue;
        // In branch view the filter matches the relative path, so "deep" finds
        // a directory and everything under it.
        if (filter_matches(entry_rel(p, i), p.filter)) p.view.push_back(i);
    }
    snprintf(p.filter_applied, sizeof(p.filter_applied), "%s", p.filter);
}

// Row helpers. With no filter `view` is the identity map, so these behave
// exactly like plain index arithmetic. `view` is ascending, hence the binary
// searches.
static int row_pos(const Pane& p, int idx) {
    return (int)(std::lower_bound(p.view.begin(), p.view.end(), idx) - p.view.begin());
}
static int entry_at_row(const Pane& p, int row) {
    if (row < 0 || row >= (int)p.view.size()) return -1;
    return p.view[row];
}
static int first_visible(const Pane& p) { return entry_at_row(p, 0); }
static int last_visible(const Pane& p)  { return entry_at_row(p, (int)p.view.size() - 1); }
static bool row_visible(const Pane& p, int idx) {
    if (p.view.size() == (size_t)p.entry_count) return true;   // nothing filtered out
    return std::binary_search(p.view.begin(), p.view.end(), idx);
}
// Next/previous visible entry from `from`; -1 when there is none. A cursor
// sitting on a hidden entry still walks to the nearest visible neighbour.
static int nav_step(const Pane& p, int from, int dir) {
    if (p.view.empty()) return -1;
    if (dir > 0) {
        auto it = std::upper_bound(p.view.begin(), p.view.end(), from);
        return it == p.view.end() ? -1 : *it;
    }
    auto it = std::lower_bound(p.view.begin(), p.view.end(), from);
    if (it == p.view.begin()) return -1;
    return *--it;
}
static int nav_page(const Pane& p, int from, int dir, int rows) {
    return entry_at_row(p, row_pos(p, from) + dir * rows);
}

static void toggle_mark(Pane& p, int idx) {
    if (idx < 0 || idx >= p.entry_count || idx >= (int)p.marked.size()) return;
    if (strcmp(p.entries[idx]->name, "..") == 0) return;
    p.marked[idx] = !p.marked[idx];
}

// Mark every visible entry in [a,b] (skips ".."); used by shift+click.
static void mark_range(Pane& p, int a, int b) {
    if (a < 0) a = b;
    int lo = a < b ? a : b;
    int hi = a < b ? b : a;
    if (lo < 0) lo = 0;
    if (hi >= p.entry_count) hi = p.entry_count - 1;
    for (int j = lo; j <= hi; j++)
        if (j < (int)p.marked.size() && strcmp(p.entries[j]->name, "..") != 0 && row_visible(p, j))
            p.marked[j] = true;
}

// Toggle every visible entry in [a,b]; used by shift+arrows so walking back
// over a marked range unmarks it.
static void toggle_range(Pane& p, int a, int b) {
    if (a < 0) a = b;
    int lo = a < b ? a : b;
    int hi = a < b ? b : a;
    if (lo < 0) lo = 0;
    if (hi >= p.entry_count) hi = p.entry_count - 1;
    for (int j = lo; j <= hi; j++)
        if (row_visible(p, j)) toggle_mark(p, j);
}

// Which entries a bulk mark operation applies to. TC distinguishes the
// numpad forms: plain/Ctrl forms cover files+dirs, the Shift+minus forms
// touch files only.
enum MarkScope { SCOPE_ALL = 0, SCOPE_FILES, SCOPE_DIRS };

static bool in_scope(const FileEntry* e, MarkScope scope) {
    bool dir = is_directory(e->name, e->mode);
    if (scope == SCOPE_FILES) return !dir;
    if (scope == SCOPE_DIRS)  return dir;
    return true;
}

// TC Num/ ("Markierung wiederherstellen"): bulk mark operations snapshot the
// mark set first so it can be walked back.
static void push_mark_undo(Pane& p) {
    p.mark_undo.push_back(p.marked);
    if (p.mark_undo.size() > 16) p.mark_undo.erase(p.mark_undo.begin());
}

static void restore_marks(Pane& p) {
    if (p.mark_undo.empty()) return;
    MarkSet snap = p.mark_undo.back();
    p.mark_undo.pop_back();
    if (snap.size() == p.marked.size()) p.marked = snap;
}

static void select_all_marks(Pane& p, MarkScope scope = SCOPE_ALL) {
    push_mark_undo(p);
    for (int i = 0; i < (int)p.marked.size(); i++)
        if (strcmp(p.entries[i]->name, "..") != 0 && in_scope(p.entries[i], scope))
            p.marked[i] = true;
}

static void clear_marks(Pane& p, MarkScope scope = SCOPE_ALL) {
    push_mark_undo(p);
    for (int i = 0; i < (int)p.marked.size(); i++)
        if (in_scope(p.entries[i], scope)) p.marked[i] = false;
}

static void invert_marks(Pane& p, MarkScope scope = SCOPE_ALL) {
    push_mark_undo(p);
    for (int i = 0; i < (int)p.marked.size(); i++)
        if (strcmp(p.entries[i]->name, "..") != 0 && in_scope(p.entries[i], scope))
            p.marked[i] = !p.marked[i];
}

// TC Num+ / Num-: set the mark at the cursor, then step the cursor down one
// row. Num+ marks, Num- unmarks ("Markierung erweitern / verkleinern").
static void step_marks(Pane& p, bool mark, MarkScope scope = SCOPE_ALL) {
    if (p.selected < 0 || p.selected >= p.entry_count) return;
    if (p.selected < (int)p.marked.size()) {
        FileEntry* e = p.entries[p.selected];
        if (strcmp(e->name, "..") != 0 && in_scope(e, scope))
            p.marked[p.selected] = mark;
    }
    int nxt = nav_step(p, p.selected, 1);
    if (nxt >= 0) { p.selected = nxt; p.scroll_request = true; }
    p.anchor = p.selected;
}

// Mark/unmark every non-directory entry sharing the cursor entry's extension
// (TC Alt+Num+ / Alt+Num-).
static void mark_same_ext(Pane& p, bool mark) {
    if (p.selected < 0 || p.selected >= p.entry_count) return;
    FileEntry* cur = p.entries[p.selected];
    if (is_directory(cur->name, cur->mode)) return;
    push_mark_undo(p);
    const char* dot = strrchr(cur->name, '.');
    std::string ext = dot ? dot : "";   // ".txt"; empty = no extension
    for (int i = 0; i < (int)p.marked.size(); i++) {
        FileEntry* e = p.entries[i];
        if (strcmp(e->name, "..") == 0 || is_directory(e->name, e->mode)) continue;
        const char* d2 = strrchr(e->name, '.');
        std::string e2 = d2 ? d2 : "";
        if (strcasecmp(e2.c_str(), ext.c_str()) == 0) p.marked[i] = mark;
    }
}

// ---------------------------------------------------------------------------
// Directory listing + history + tabs
// ---------------------------------------------------------------------------

void refresh_pane(Pane* pane, const char* path) {
    bool dir_changed = strcmp(pane->current_dir, path) != 0;
    free_entries(pane->entries, pane->entry_count);
    strncpy(pane->current_dir, path, sizeof(pane->current_dir) - 1);
    pane->current_dir[sizeof(pane->current_dir) - 1] = '\0';
    pane->selected = 0;
    pane->anchor = 0;
    list_directory(path, &pane->entries, &pane->entry_count);
    pane->list_gen++;
    pane->marked.assign(pane->entry_count, false);
    pane->dir_size.assign(pane->entry_count, -1);
    // Mark snapshots and dir sizes are index-based, so a reload invalidates them.
    pane->mark_undo.clear();
    // TC drops the quick filter when the directory changes; a plain refresh of
    // the same directory keeps it.
    if (dir_changed) {
        pane->filter[0] = '\0';
        pane->filter_active = false;
        pane->filter_field_active = false;
    }
    // Branch view is a snapshot of a subtree; navigating ends it.
    pane->list_mode = LIST_NORMAL;
    pane->rel.clear();
    rebuild_view(*pane);
}

// TC: Space on an unmarked directory sums its contents so the size column can
// show a real number instead of "-". Alt+Shift+Enter does it for every dir.
static void compute_dir_size(Pane& pane, int idx) {
    if (idx < 0 || idx >= pane.entry_count || idx >= (int)pane.dir_size.size()) return;
    FileEntry* e = pane.entries[idx];
    if (!is_directory(e->name, e->mode) || strcmp(e->name, "..") == 0) return;
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", pane.current_dir, entry_rel(pane, idx));
    long long bytes = 0; long nfiles = 0, ndirs = 0;
    fs_count_tree(full, &bytes, &nfiles, &ndirs);
    pane.dir_size[idx] = bytes;
}

void app_panes_refresh_left(AppPanes* app) {
    refresh_pane(&app->left, app->left.current_dir);
}

void app_panes_refresh_right(AppPanes* app) {
    refresh_pane(&app->right, app->right.current_dir);
}

// Remember which entry name is selected in the pane's current directory so
// the cursor can be restored when the directory is re-entered.
static void remember_selection(Pane* p) {
    if (p->selected < 0 || p->selected >= p->entry_count) return;
    const char* name = p->entries[p->selected]->name;
    for (int i = 0; i < p->sel_mem_count; i++) {
        if (strcmp(p->sel_memory[i].dir, p->current_dir) == 0) {
            snprintf(p->sel_memory[i].name, sizeof(p->sel_memory[i].name), "%s", name);
            return;
        }
    }
    if (p->sel_mem_count < MAX_SEL_MEMORY) {
        snprintf(p->sel_memory[p->sel_mem_count].dir, sizeof(p->sel_memory[0].dir), "%s", p->current_dir);
        snprintf(p->sel_memory[p->sel_mem_count].name, sizeof(p->sel_memory[0].name), "%s", name);
        p->sel_mem_count++;
    }
}

// Restore the remembered cursor position for the pane's current directory.
static void restore_selection(Pane* p) {
    for (int i = 0; i < p->sel_mem_count; i++) {
        if (strcmp(p->sel_memory[i].dir, p->current_dir) != 0) continue;
        for (int j = 0; j < p->entry_count; j++) {
            if (strcmp(p->entries[j]->name, p->sel_memory[i].name) == 0) {
                p->selected = j;
                p->scroll_request = true;
                return;
            }
        }
        return; // remembered name no longer exists: keep top of list
    }
}

// Record a navigation in the pane's history (drop forward entries).
static void push_history(Pane* p, const char* new_dir) {
    if (p->history_count == 0) {
        snprintf(p->history[p->history_count++], sizeof(p->history[0]), "%s", p->current_dir);
        p->history_pos = 0;
    }
    if (p->history_pos < p->history_count - 1)
        p->history_count = p->history_pos + 1;   // truncate forward entries
    if (strcmp(p->history[p->history_pos], new_dir) == 0) return;
    if (p->history_count >= MAX_DIR_HISTORY) {
        memmove(p->history[0], p->history[1], sizeof(p->history[0]) * (MAX_DIR_HISTORY - 1));
        p->history_count = MAX_DIR_HISTORY - 1;
        p->history_pos = p->history_count - 1;
    }
    snprintf(p->history[p->history_count], sizeof(p->history[0]), "%s", new_dir);
    p->history_pos = p->history_count;
    p->history_count++;
}

// Navigate a pane to a path: records history and updates the active tab.
static void navigate_to(AppPanes* app, Pane* pane, const char* path) {
    if (strcmp(path, pane->current_dir) != 0) remember_selection(pane);
    push_history(pane, path);
    refresh_pane(pane, path);
    restore_selection(pane);
    if (pane->tab_count > 0)
        snprintf(pane->tabs[pane->tab_active].dir, sizeof(pane->tabs[0].dir), "%s", pane->current_dir);
    (void)app;
}

static void history_back(AppPanes* app, Pane* pane) {
    if (pane->history_pos <= 0) return;
    remember_selection(pane);
    pane->history_pos--;
    refresh_pane(pane, pane->history[pane->history_pos]);
    restore_selection(pane);
    if (pane->tab_count > 0)
        snprintf(pane->tabs[pane->tab_active].dir, sizeof(pane->tabs[0].dir), "%s", pane->current_dir);
    (void)app;
}

static void history_forward(AppPanes* app, Pane* pane) {
    if (pane->history_pos >= pane->history_count - 1) return;
    remember_selection(pane);
    pane->history_pos++;
    refresh_pane(pane, pane->history[pane->history_pos]);
    restore_selection(pane);
    if (pane->tab_count > 0)
        snprintf(pane->tabs[pane->tab_active].dir, sizeof(pane->tabs[0].dir), "%s", pane->current_dir);
    (void)app;
}

static void tab_new(Pane* pane, bool activate) {
    if (pane->tab_count >= MAX_TABS || pane->tab_count == 0) return;
    snprintf(pane->tabs[pane->tab_count].dir, sizeof(pane->tabs[0].dir), "%s", pane->current_dir);
    if (activate) pane->tab_active = pane->tab_count;
    pane->tab_count++;
}

static void tab_close(Pane* pane) {
    if (pane->tab_count <= 1) return;
    int t = pane->tab_active;
    for (int i = t; i < pane->tab_count - 1; i++)
        pane->tabs[i] = pane->tabs[i + 1];
    pane->tab_count--;
    if (pane->tab_active >= pane->tab_count) pane->tab_active = pane->tab_count - 1;
    refresh_pane(pane, pane->tabs[pane->tab_active].dir);
}

static void tab_switch(Pane* pane, int t) {
    if (t < 0 || t >= pane->tab_count || t == pane->tab_active) return;
    snprintf(pane->tabs[pane->tab_active].dir, sizeof(pane->tabs[0].dir), "%s", pane->current_dir);
    pane->tab_active = t;
    refresh_pane(pane, pane->tabs[t].dir);
}

static void tab_cycle(Pane* pane, int delta) {    if (pane->tab_count < 2) return;
    int t = (pane->tab_active + delta + pane->tab_count) % pane->tab_count;
    tab_switch(pane, t);
}

// ---------------------------------------------------------------------------
// Pane swap (TC Ctrl+U / Ctrl+Shift+U)
// ---------------------------------------------------------------------------

// Marks and the cursor are index-based, so a swap has to carry them across by
// name: collect before the reload, re-apply after it.
static void collect_marks_by_name(const Pane& p, std::vector<std::string>& out, char* cursor, size_t n) {
    out.clear();
    if (cursor) cursor[0] = '\0';
    for (int i = 0; i < p.entry_count && i < (int)p.marked.size(); i++)
        if (p.marked[i]) out.push_back(p.entries[i]->name);
    if (cursor && p.selected >= 0 && p.selected < p.entry_count)
        snprintf(cursor, n, "%s", p.entries[p.selected]->name);
}

static void apply_marks_by_name(Pane& p, const std::vector<std::string>& names, const char* cursor) {
    for (int i = 0; i < p.entry_count; i++) {
        for (const std::string& nm : names)
            if (nm == p.entries[i]->name) { p.marked[i] = true; break; }
    }
    if (cursor && cursor[0]) {
        for (int i = 0; i < p.entry_count; i++)
            if (strcmp(p.entries[i]->name, cursor) == 0) {
                p.selected = i; p.scroll_request = true; break;
            }
    }
}

// TC Ctrl+Shift+W: close every tab except the one being shown.
static void tab_close_all(Pane* pane) {
    if (pane->tab_count <= 1) return;
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", pane->current_dir);
    snprintf(pane->tabs[0].dir, sizeof(pane->tabs[0].dir), "%s", dir);
    pane->tab_count = 1;
    pane->tab_active = 0;
    refresh_pane(pane, dir);
}

// TC Ctrl+Up / Ctrl+Shift+Down: the directory under the cursor in a new tab,
// in this pane or in the other one.
static void open_cursor_in_new_tab(AppPanes* app, Pane* pane, bool activate) {
    if (pane->selected < 0 || pane->selected >= pane->entry_count) return;
    FileEntry* e = pane->entries[pane->selected];
    if (strcmp(e->name, "..") == 0 || !is_directory(e->name, e->mode)) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Not a directory: %s", e->name);
        return;
    }
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", pane->current_dir, entry_rel(*pane, pane->selected));
    tab_new(pane, activate);
    navigate_to(app, pane, full);
}

// TC Ctrl+Left/Right: show the directory under the cursor in the other pane.
static void open_cursor_in_other_pane(AppPanes* app, Pane* src, Pane* dst) {
    if (src->selected < 0 || src->selected >= src->entry_count) return;
    FileEntry* e = src->entries[src->selected];
    if (strcmp(e->name, "..") == 0 || !is_directory(e->name, e->mode)) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Not a directory: %s", e->name);
        return;
    }
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", src->current_dir, entry_rel(*src, src->selected));
    navigate_to(app, dst, full);
}

static void swap_panes(AppPanes* app, bool swap_tabs_too) {
    std::vector<std::string> lm, rm;
    char lc[256] = "", rc[256] = "";
    collect_marks_by_name(app->left, lm, lc, sizeof(lc));
    collect_marks_by_name(app->right, rm, rc, sizeof(rc));

    char ldir[4096], rdir[4096];
    snprintf(ldir, sizeof(ldir), "%s", app->left.current_dir);
    snprintf(rdir, sizeof(rdir), "%s", app->right.current_dir);

    navigate_to(app, &app->left, rdir);
    navigate_to(app, &app->right, ldir);

    apply_marks_by_name(app->left, rm, rc);
    apply_marks_by_name(app->right, lm, lc);

    if (swap_tabs_too) {
        PaneTab t[MAX_TABS];
        int tc = app->left.tab_count, ac = app->left.tab_active;
        memcpy(t, app->left.tabs, sizeof(t));
        app->left.tab_count = app->right.tab_count;
        app->left.tab_active = app->right.tab_active;
        memcpy(app->left.tabs, app->right.tabs, sizeof(app->left.tabs));
        app->right.tab_count = tc;
        app->right.tab_active = ac;
        memcpy(app->right.tabs, t, sizeof(app->right.tabs));
        // The active tab's dir must track the pane it now belongs to.
        if (app->left.tab_count > 0)
            snprintf(app->left.tabs[app->left.tab_active].dir, sizeof(PaneTab::dir), "%s", app->left.current_dir);
        if (app->right.tab_count > 0)
            snprintf(app->right.tabs[app->right.tab_active].dir, sizeof(PaneTab::dir), "%s", app->right.current_dir);
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "Swapped panes: %s <-> %s", ldir, rdir);
}

// ---------------------------------------------------------------------------
// Hotlist (bookmarks), persisted under ~/.config/mc-clone/hotlist
// ---------------------------------------------------------------------------

static const char* hotlist_file() {
    static char path[4096];
    const char* home = getenv("HOME");
    snprintf(path, sizeof(path), "%s/.config/mc-clone/hotlist", home ? home : ".");
    return path;
}

static void hotlist_load(AppPanes* app) {
    FILE* f = fopen(hotlist_file(), "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n) app->hotlist.push_back(line);
    }
    fclose(f);
}

static void hotlist_save(AppPanes* app) {
    const char* home = getenv("HOME");
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/.config", home ? home : ".");
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.config/mc-clone", home ? home : ".");
    mkdir(dir, 0755);
    FILE* f = fopen(hotlist_file(), "w");
    if (!f) return;
    for (const std::string& s : app->hotlist) fprintf(f, "%s\n", s.c_str());
    fclose(f);
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

static bool ci_contains(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (const char* h = hay; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static void search_walk(const char* dir, const char* pattern, std::vector<std::string>& out,
                        int depth, bool show_hidden) {
    if (depth > 12 || out.size() >= 1000) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (!show_hidden && de->d_name[0] == '.') continue;
        char full[4100];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (ci_contains(de->d_name, pattern)) out.push_back(full);
        if (S_ISDIR(st.st_mode)) search_walk(full, pattern, out, depth + 1, show_hidden);
        if (out.size() >= 1000) break;
    }
    closedir(d);
}

static void run_search(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    app->search_results.clear();
    search_walk(active.current_dir, app->search_pattern, app->search_results, 0, app->show_hidden);
    app->search_done = true;
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

static void human_size(off_t s, char* out, size_t n) {
    if (s >= 1000000000LL)  snprintf(out, n, "%4.1fG", (double)s / 1e9);
    else if (s >= 1000000)  snprintf(out, n, "%4.1fM", (double)s / 1e6);
    else if (s >= 1000)     snprintf(out, n, "%4.1fK", (double)s / 1e3);
    else                    snprintf(out, n, "%4lld", (long long)s);
}

static void fmt_date(time_t t, char* out, size_t n) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, n, "%m-%d %H:%M", &tmv);
}

// ---------------------------------------------------------------------------
// Command line: builtins + popen, output shown in a window
// ---------------------------------------------------------------------------

static void run_shell_and_show(AppPanes* app, const char* shell_cmd) {
    FILE* p = popen(shell_cmd, "r");
    if (!p) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Failed to run: %s", strerror(errno));
        return;
    }
    size_t total = 0;
    size_t cap = sizeof(app->output_text) - 1;
    while (total < cap) {
        size_t n = fread(app->output_text + total, 1, cap - total, p);
        if (!n) break;
        total += n;
    }
    app->output_text[total] = '\0';
    pclose(p);
    app->output_open = true;
}

static void exec_command(AppPanes* app) {
    char* c = app->cmd;
    while (*c == ' ') c++;
    if (!*c) return;

    if (app->cmd_hist_count < (int)(sizeof(app->cmd_history) / sizeof(app->cmd_history[0])))
        snprintf(app->cmd_history[app->cmd_hist_count++], sizeof(app->cmd_history[0]), "%s", c);
    app->cmd_hist_pos = app->cmd_hist_count;

    Pane& active = app->left_focus ? app->left : app->right;

    if (strcmp(c, "cd") == 0 || strncmp(c, "cd ", 3) == 0) {
        const char* arg = c + 2;
        while (*arg == ' ') arg++;
        char target[4096];
        const char* home = getenv("HOME");
        if (!*arg) {
            snprintf(target, sizeof(target), "%s", home ? home : "/");
        } else if (arg[0] == '~' && (arg[1] == '/' || arg[1] == '\0')) {
            snprintf(target, sizeof(target), "%s%s", home ? home : "", arg + 1);
        } else if (arg[0] == '/') {
            snprintf(target, sizeof(target), "%s", arg);
        } else {
            snprintf(target, sizeof(target), "%s/%s", active.current_dir, arg);
        }
        DIR* probe = opendir(target);
        if (probe) {
            closedir(probe);
            navigate_to(app, &active, target);
        } else {
            snprintf(app->status_msg, sizeof(app->status_msg), "cd: no such directory: %s", target);
        }
    } else if (strcmp(c, "exit") == 0 || strcmp(c, "quit") == 0) {
        app->quit_request = true;
    } else {
        char shell_cmd[600];
        snprintf(shell_cmd, sizeof(shell_cmd), "%s 2>&1", c);
        run_shell_and_show(app, shell_cmd);
    }
    app->cmd[0] = '\0';
}

// ---------------------------------------------------------------------------
// File operations with confirmation
// ---------------------------------------------------------------------------

static int run_on_targets(AppPanes* app, Pane& pane, bool (*act)(const char*, const char*), const char* dst_dir, const char* name_override) {
    int ok = 0;
    int marks = marked_count(pane);
    for (int i = 0; i < pane.entry_count; i++) {
        if (marks == 0 && i != pane.selected) continue;
        if (marks > 0 && !pane.marked[i]) continue;
        FileEntry* e = pane.entries[i];
        if (strcmp(e->name, "..") == 0) continue;
        char src[4100];
        snprintf(src, sizeof(src), "%s/%s", pane.current_dir, entry_rel(pane, i));
        const char* nm = name_override ? name_override : e->name;
        char dst[4100];
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, nm);
        if (act(src, dst)) ok++;
    }
    return ok;
}

static int count_targets(AppPanes* app, Pane& pane) {
    int marks = marked_count(pane);
    if (marks > 0) return marks;
    if (pane.selected < pane.entry_count && strcmp(pane.entries[pane.selected]->name, "..") != 0) return 1;
    return 0;
}

static int count_existing_targets(AppPanes* app, Pane& pane, const char* dst_dir) {
    int n = 0;
    int marks = marked_count(pane);
    for (int i = 0; i < pane.entry_count; i++) {
        if (marks == 0 && i != pane.selected) continue;
        if (marks > 0 && !pane.marked[i]) continue;
        FileEntry* e = pane.entries[i];
        if (strcmp(e->name, "..") == 0) continue;
        char dst[4100];
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, e->name);
        if (fs_exists(dst)) n++;
    }
    return n;
}

// Progress helpers are defined below with the copy/move operations.
static bool copy_progress_cb(long long done, long long total, int fd, int ft, void* user);
static void prog_begin(AppPanes* app, Pane& active, Pane& other, bool is_move);

static void do_delete(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    bool to_trash = app->delete_trash != 0;
    prog_begin(app, active, active, false);
    app->prog_is_delete = true;
    int ok = 0;
    int marks = marked_count(active);
    for (int i = 0; i < active.entry_count; i++) {
        if (marks == 0 && i != active.selected) continue;
        if (marks > 0 && !active.marked[i]) continue;
        FileEntry* e = active.entries[i];
        if (strcmp(e->name, "..") == 0) continue;
        char src[4100];
        snprintf(src, sizeof(src), "%s/%s", active.current_dir, entry_rel(active, i));
        bool removed;
        if (to_trash) {
            // A trash helper is a rename, so it has no per-file progress to pump.
            removed = fs_trash(src) || fs_delete_entry_progress(src, copy_progress_cb, app);
        } else {
            removed = fs_delete_entry_progress(src, copy_progress_cb, app);
        }
        if (removed) ok++;
        if (app->quit_request) break;
    }
    app->prog_active = false;
    if (ok) snprintf(app->status_msg, sizeof(app->status_msg), to_trash ? "Trashed %d entries" : "Deleted %d entries", ok);
    else    snprintf(app->status_msg, sizeof(app->status_msg), "Delete failed: %s", strerror(errno));
    refresh_pane(&active, active.current_dir);
}

static void request_delete(AppPanes* app, bool force_direct) {
    Pane& active = app->left_focus ? app->left : app->right;
    int n = count_targets(app, active);
    if (n == 0) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to delete");
        return;
    }
    // TC splits "direkt" and "Papierkorb": Shift always deletes for good, and
    // the prompt has to say which one is about to happen.
    app->delete_trash = (app->trash_mode && !force_direct && fs_trash_available()) ? 1 : 0;
    app->confirm = CONF_DELETE;
    if (app->delete_trash)
        snprintf(app->confirm_text, sizeof(app->confirm_text),
                 "Move %d entrie%s to the trash ?", n, n == 1 ? "y" : "s");
    else
        snprintf(app->confirm_text, sizeof(app->confirm_text),
                 "Delete %d entrie%s in %s ?", n, n == 1 ? "y" : "s", active.current_dir);
}

void app_panes_set_sdl_window(AppPanes* app, void* window) {
    app->sdl_window = window;
}

// ---------------------------------------------------------------------------
// Copy/move progress dialog. The copy runs synchronously; between files we
// pump a minimal ImGui frame that shows only this dialog, so the window
// stays responsive and shows live numbers.
// ---------------------------------------------------------------------------

static void render_progress_dialog(AppPanes* app) {
    if (!app->prog_active) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 sz(560, 230);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f,
                                   vp->Pos.y + (vp->Size.y - sz.y) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    const char* title = app->prog_is_delete ? "Deleting"
                      : (app->prog_is_move  ? "Moving" : "Copying");
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

    ImGui::Text("%s", app->prog_src);
    if (!app->prog_is_delete) {
        ImGui::SameLine();
        ImGui::TextUnformatted("->");
        ImGui::SameLine();
        ImGui::Text("%s", app->prog_dst);
    }

    float frac = app->prog_total > 0
               ? (float)((double)app->prog_done / (double)app->prog_total) : 0.0f;
    char overlay[32];
    snprintf(overlay, sizeof(overlay), "%.1f%%", frac * 100.0f);
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);

    char done_s[32], total_s[32], rem_s[32], rate_s[32];
    human_size((off_t)app->prog_done, done_s, sizeof(done_s));
    human_size((off_t)app->prog_total, total_s, sizeof(total_s));
    human_size((off_t)(app->prog_total - app->prog_done), rem_s, sizeof(rem_s));

    double now = SDL_GetTicks() / 1000.0;
    double elapsed = now - app->prog_t0;
    double rate = elapsed > 0.05 ? (double)app->prog_done / elapsed : 0.0;
    human_size((off_t)rate, rate_s, sizeof(rate_s));

    ImGui::Text("Files:      %d / %d", app->prog_files_done, app->prog_files_total);
    ImGui::Text("%-10s %s of %s   Remaining: %s",
                app->prog_is_delete ? "Deleted:" : "Copied:", done_s, total_s, rem_s);
    ImGui::Text("Throughput: %s/s   Elapsed: %.1fs", rate_s, elapsed);
    if (rate > 1.0 && app->prog_total > app->prog_done) {
        double eta = (double)(app->prog_total - app->prog_done) / rate;
        ImGui::Text("ETA:        %.0fs", eta);
    }
    ImGui::End();
}

static void pump_progress_ui(AppPanes* app) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) app->quit_request = true;
        ImGui_ImplSDL3_ProcessEvent(&e);
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    render_progress_dialog(app);
    ImGui::Render();

    SDL_Window* w = (SDL_Window*)app->sdl_window;
    if (!w) return;
    int dw, dh;
    SDL_GetWindowSize(w, &dw, &dh);
    glViewport(0, 0, dw, dh);
    const ImVec4& bg = mc_theme_colors()->window_bg;
    glClearColor(bg.x, bg.y, bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(w);
}

static bool copy_progress_cb(long long done, long long total, int fd, int ft, void* user) {
    AppPanes* app = (AppPanes*)user;
    app->prog_done = done;
    app->prog_total = total;
    app->prog_files_done = fd;
    app->prog_files_total = ft;
    double now = SDL_GetTicks() / 1000.0;
    if (now - app->prog_last_pump >= 0.05) {   // ~20 fps refresh
        app->prog_last_pump = now;
        pump_progress_ui(app);
    }
    return !app->quit_request;                 // closing the window aborts
}

static void prog_begin(AppPanes* app, Pane& active, Pane& other, bool is_move) {
    app->prog_active = true;
    app->prog_is_move = is_move;
    app->prog_is_delete = false;
    snprintf(app->prog_src, sizeof(app->prog_src), "%s", active.current_dir);
    snprintf(app->prog_dst, sizeof(app->prog_dst), "%s", other.current_dir);
    app->prog_done = 0;
    app->prog_total = 0;
    app->prog_files_done = 0;
    app->prog_files_total = 0;
    app->prog_t0 = SDL_GetTicks() / 1000.0;
    app->prog_last_pump = 0.0;
}

static int run_on_targets_prog(AppPanes* app, Pane& pane, bool is_move,
                               const char* dst_dir, fs_progress_fn cb, void* user) {
    int ok = 0;
    int marks = marked_count(pane);
    for (int i = 0; i < pane.entry_count; i++) {
        if (marks == 0 && i != pane.selected) continue;
        if (marks > 0 && !pane.marked[i]) continue;
        FileEntry* e = pane.entries[i];
        if (strcmp(e->name, "..") == 0) continue;
        char src[4100];
        snprintf(src, sizeof(src), "%s/%s", pane.current_dir, entry_rel(pane, i));
        char dst[4100];
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, e->name);
        bool r = is_move ? fs_move_entry_progress(src, dst, cb, user)
                         : fs_copy_entry_progress(src, dst, cb, user);
        if (r) ok++;
        if (app->quit_request) break;
    }
    return ok;
}

static void do_copy_now(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;
    prog_begin(app, active, other, false);
    int ok = run_on_targets_prog(app, active, false, other.current_dir, copy_progress_cb, app);
    app->prog_active = false;
    if (ok) snprintf(app->status_msg, sizeof(app->status_msg), "Copied %d entries to %s", ok, other.current_dir);
    else    snprintf(app->status_msg, sizeof(app->status_msg), "Copy failed: %s", strerror(errno));
    refresh_pane(&other, other.current_dir);
}

static void do_move_now(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;

    // Remember the slot being vacated so the cursor can land on the entry
    // that moves up into it (or the one above if the list got shorter).
    int slot = active.selected;
    if (marked_count(active) > 0) {
        for (int i = 0; i < active.entry_count; i++)
            if (active.marked[i]) { slot = i; break; }
    }

    prog_begin(app, active, other, true);
    int ok = run_on_targets_prog(app, active, true, other.current_dir, copy_progress_cb, app);
    app->prog_active = false;
    if (ok) snprintf(app->status_msg, sizeof(app->status_msg), "Moved %d entries to %s", ok, other.current_dir);
    else    snprintf(app->status_msg, sizeof(app->status_msg), "Move failed: %s", strerror(errno));
    refresh_pane(&active, active.current_dir);
    refresh_pane(&other, other.current_dir);

    if (ok && active.entry_count > 0) {
        if (slot > active.entry_count - 1) slot = active.entry_count - 1;
        if (slot < 0) slot = 0;
        active.selected = slot;
        active.anchor = slot;
        active.scroll_request = true;
    }
}

static void do_copy(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;
    int existing = count_existing_targets(app, active, other.current_dir);
    if (existing > 0) {
        app->confirm = CONF_OVERWRITE;
        snprintf(app->confirm_text, sizeof(app->confirm_text),
                 "%d target%s already exist in %s. Overwrite?", existing, existing == 1 ? "" : "s", other.current_dir);
        return;
    }
    do_copy_now(app);
}

static void do_move(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;
    int existing = count_existing_targets(app, active, other.current_dir);
    if (existing > 0) {
        app->confirm = CONF_OVERWRITE_MOVE;
        snprintf(app->confirm_text, sizeof(app->confirm_text),
                 "%d target%s already exist in %s. Overwrite?", existing, existing == 1 ? "" : "s", other.current_dir);
        return;
    }
    do_move_now(app);
}

// ---------------------------------------------------------------------------
// Internal file clipboard (TC Ctrl+C / Ctrl+X / Ctrl+V)
// ---------------------------------------------------------------------------

// The operation targets: everything marked, or just the entry under the cursor
// when nothing is marked. Same rule as run_on_targets_prog().
static StringList target_paths(const Pane& pane) {
    StringList out;
    int marks = marked_count(pane);
    for (int i = 0; i < pane.entry_count; i++) {
        if (marks == 0 && i != pane.selected) continue;
        if (marks > 0 && !pane.marked[i]) continue;
        if (strcmp(pane.entries[i]->name, "..") == 0) continue;
        out.push_back(std::string(pane.current_dir) + "/" + entry_rel(pane, i));
    }
    return out;
}

static const char* path_basename(const std::string& p) {
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p.c_str() : p.c_str() + slash + 1;
}

static void path_parent_dir(const std::string& p, char* out, size_t cap) {
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) { snprintf(out, cap, "."); return; }
    snprintf(out, cap, "%.*s", (int)slash, p.c_str());
}

static void clipboard_set(AppPanes* app, Pane& pane, bool cut) {
    StringList paths = target_paths(pane);
    if (paths.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to %s",
                 cut ? "cut" : "copy");
        return;
    }
    app->clipboard.paths = paths;
    app->clipboard.cut = cut;
    snprintf(app->status_msg, sizeof(app->status_msg),
             "%d entr%s on the clipboard (%s)", (int)paths.size(),
             paths.size() == 1 ? "y" : "ies", cut ? "cut" : "copy");
}

static int count_existing_paths(const char* dst_dir, const StringList& paths) {
    int n = 0;
    for (size_t i = 0; i < paths.size(); i++) {
        char dst[4100];
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, path_basename(paths[i]));
        if (fs_exists(dst)) n++;
    }
    return n;
}

static void do_paste_now(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other  = app->left_focus ? app->right : app->left;
    if (app->clipboard.paths.empty()) return;

    prog_begin(app, active, active, app->clipboard.cut);
    path_parent_dir(app->clipboard.paths[0], app->prog_src, sizeof(app->prog_src));
    snprintf(app->prog_dst, sizeof(app->prog_dst), "%s", active.current_dir);

    int ok = 0;
    StringList failed;   // a cut keeps whatever did not move so it can be retried
    for (size_t i = 0; i < app->clipboard.paths.size(); i++) {
        const std::string& src = app->clipboard.paths[i];
        char dst[4100];
        snprintf(dst, sizeof(dst), "%s/%s", active.current_dir, path_basename(src));
        bool r = app->clipboard.cut
                     ? fs_move_entry_progress(src.c_str(), dst, copy_progress_cb, app)
                     : fs_copy_entry_progress(src.c_str(), dst, copy_progress_cb, app);
        if (r) ok++;
        else failed.push_back(src);
        if (app->quit_request) {
            for (size_t j = i + 1; j < app->clipboard.paths.size(); j++) failed.push_back(app->clipboard.paths[j]);
            break;
        }
    }
    app->prog_active = false;

    // TC: a copy stays on the clipboard so it can be pasted again; a cut is
    // consumed, keeping only the entries that failed to move.
    if (app->clipboard.cut) app->clipboard.paths = failed;

    snprintf(app->status_msg, sizeof(app->status_msg),
             "%s %d entr%s into %s", app->clipboard.cut ? "Moved" : "Copied",
             ok, ok == 1 ? "y" : "ies", active.current_dir);
    refresh_pane(&active, active.current_dir);
    refresh_pane(&other, other.current_dir);
}

static void do_paste(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (app->clipboard.paths.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Clipboard is empty");
        return;
    }
    int existing = count_existing_paths(active.current_dir, app->clipboard.paths);
    if (existing > 0) {
        app->confirm = CONF_OVERWRITE_PASTE;
        snprintf(app->confirm_text, sizeof(app->confirm_text),
                 "%d target%s already exist in %s. Overwrite?", existing,
                 existing == 1 ? "" : "s", active.current_dir);
        return;
    }
    do_paste_now(app);
}

static void confirm_yes(AppPanes* app) {
    switch (app->confirm) {
    case CONF_DELETE:         do_delete(app); break;
    case CONF_OVERWRITE:      do_copy_now(app); break;
    case CONF_OVERWRITE_MOVE: do_move_now(app); break;
    case CONF_OVERWRITE_PASTE: do_paste_now(app); break;
    default: break;
    }
    app->confirm = CONF_NONE;
    app->confirm_text[0] = '\0';
}

// Deferred actions from menu/context-menu/dialog widgets: these fire inside
// the ImGui frame, but copy/move pump their own frames, so they must run
// between frames instead.
void app_panes_run_pending(AppPanes* app) {
    if (app->pending_confirm_yes) {
        app->pending_confirm_yes = false;
        confirm_yes(app);
    }
    if (app->pending_op) {
        int op = app->pending_op;
        app->pending_op = 0;
        if (op == 1)      do_copy(app);
        else if (op == 2) do_move(app);
        else if (op == 3) do_paste(app);
    }
}

// ---------------------------------------------------------------------------
// Viewer + entry activation
// ---------------------------------------------------------------------------

static char viewer_buf[256 * 1024];
static float viewer_scroll = 0.0f;
static bool  viewer_dirty = false;

static void open_viewer(AppPanes* app, const char* path) {
    snprintf(app->viewer_path, sizeof(app->viewer_path), "%s", path);
    long n = fs_read_preview(path, viewer_buf, sizeof(viewer_buf) - 1);
    if (n < 0) {
        // Binary: classic hex dump instead of nothing
        int hn = snprintf(viewer_buf, sizeof(viewer_buf), "(binary - hex dump of first 16384 bytes)\n\n");
        long dumped = fs_hex_dump(path, viewer_buf + hn, sizeof(viewer_buf) - (size_t)hn, 16384);
        if (dumped < 0) snprintf(viewer_buf, sizeof(viewer_buf), "(binary or unreadable)");
    }
    viewer_scroll = 0.0f;
    viewer_dirty = false;
    app->viewer_open = true;
}

// Ctrl+B: TC's branch view. The pane shows the subtree under its directory as
// one flat list; the rows stay index-parallel to entries, so the filter, the
// marks and the cursor all keep working.
static const int BRANCH_MAX_DEPTH = 6;
static const int BRANCH_MAX_ROWS = 4000;

static void branch_fill(AppPanes* app, Pane& pane, bool marked_only) {
    std::vector<FsTreeEntry> tree;
    if (marked_only) {
        int marks = marked_count(pane);
        for (int i = 0; i < pane.entry_count && (int)tree.size() < BRANCH_MAX_ROWS; i++) {
            if (marks == 0 || i >= (int)pane.marked.size() || !pane.marked[i]) continue;
            FileEntry* e = pane.entries[i];
            if (!is_directory(e->name, e->mode) || strcmp(e->name, "..") == 0) continue;
            char full[4100];
            snprintf(full, sizeof(full), "%s/%s", pane.current_dir, e->name);
            std::vector<FsTreeEntry> sub;
            fs_list_tree(full, &sub, BRANCH_MAX_DEPTH, BRANCH_MAX_ROWS - (int)tree.size());
            for (const FsTreeEntry& t : sub) {
                FsTreeEntry e2 = t;
                snprintf(e2.rel, sizeof(e2.rel), "%s/%s", e->name, t.rel);
                tree.push_back(e2);
            }
        }
        if (tree.empty()) {
            snprintf(app->status_msg, sizeof(app->status_msg), "No marked directories to branch");
            return;
        }
    } else {
        fs_list_tree(pane.current_dir, &tree, BRANCH_MAX_DEPTH, BRANCH_MAX_ROWS);
    }

    free_entries(pane.entries, pane.entry_count);
    pane.entries = nullptr;
    pane.entry_count = 0;
    pane.rel.clear();

    std::vector<FileEntry*> list;
    for (const FsTreeEntry& t : tree) {
        const char* base = strrchr(t.rel, '/');
        base = base ? base + 1 : t.rel;
        FileEntry* fe = file_entry_new(base, t.mode, t.size, t.mtime);
        if (!fe) continue;
        list.push_back(fe);
        pane.rel.push_back(t.rel);
    }
    if (!list.empty()) {
        pane.entries = (FileEntry**)malloc(list.size() * sizeof(FileEntry*));
        if (pane.entries) {
            for (size_t i = 0; i < list.size(); i++) pane.entries[i] = list[i];
            pane.entry_count = (int)list.size();
        } else {
            for (FileEntry* fe : list) file_entry_free(fe);
        }
    }
    pane.selected = 0;
    pane.anchor = 0;
    pane.marked.assign(pane.entry_count, false);
    pane.dir_size.assign(pane.entry_count, -1);
    pane.mark_undo.clear();
    pane.list_mode = LIST_BRANCH;
    rebuild_view(pane);
    snprintf(app->status_msg, sizeof(app->status_msg), "Branch view: %d rows (Ctrl+B exits)",
             pane.entry_count);
}

static void branch_toggle(AppPanes* app, Pane& pane, bool marked_only) {
    if (pane.list_mode == LIST_BRANCH) {
        refresh_pane(&pane, pane.current_dir);
        snprintf(app->status_msg, sizeof(app->status_msg), "Branch view off");
        return;
    }
    branch_fill(app, pane, marked_only);
}

// Path of a row inside its pane is handled by entry_rel(); rename and mkdir are
// not, so they stay out of branch view.
static bool branch_blocked(AppPanes* app, const Pane& pane) {
    if (pane.list_mode != LIST_BRANCH) return false;
    snprintf(app->status_msg, sizeof(app->status_msg), "Not available in branch view (Ctrl+B exits)");
    return true;
}

static void activate_entry(AppPanes* app, Pane& pane, int idx) {
    if (idx < 0 || idx >= pane.entry_count) return;
    FileEntry* e = pane.entries[idx];
    const char* rel = entry_rel(pane, idx);
    if (strcmp(e->name, "..") == 0) {
        char parentbuf[4096];
        snprintf(parentbuf, sizeof(parentbuf), "%s", pane.current_dir);
        char* parent = dirname(parentbuf);
        navigate_to(app, &pane, parent);
    } else if (is_directory(e->name, e->mode)) {
        char path[4100];
        snprintf(path, sizeof(path), "%s/%s", pane.current_dir, rel);
        navigate_to(app, &pane, path);
    } else if (is_executable(e->name, e->mode)) {
        char full[4300];
        snprintf(full, sizeof(full), "\"%s/%s\" 2>&1", pane.current_dir, rel);
        run_shell_and_show(app, full);
    } else {
        char full[4100];
        snprintf(full, sizeof(full), "%s/%s", pane.current_dir, rel);
        open_viewer(app, full);
    }
}

// F4: launch $EDITOR inside a terminal emulator so it gets a TTY.
static void spawn_terminal_editor(AppPanes* app, const char* fullpath) {
    static const char* terms[] = {
        "x-terminal-emulator", "xterm", "xfce4-terminal", "konsole", "gnome-terminal", nullptr
    };
    const char* editor = getenv("EDITOR");
    if (!editor || !*editor) editor = "vim";

    const char* chosen = nullptr;
    for (int i = 0; terms[i]; i++) {
        char probe[256];
        snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", terms[i]);
        if (system(probe) == 0) { chosen = terms[i]; break; }
    }
    if (!chosen) {
        snprintf(app->status_msg, sizeof(app->status_msg), "No terminal emulator found for %s", editor);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execlp(chosen, chosen, "-e", editor, fullpath, (char*)nullptr);
        _exit(127);
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "Edit: %s %s (in %s)", editor, fullpath, chosen);
}

// Alt+F3: hand the entry to the desktop's default handler.
static void open_with_default(AppPanes* app, const char* fullpath) {
    if (system("command -v xdg-open >/dev/null 2>&1") != 0) {
        snprintf(app->status_msg, sizeof(app->status_msg), "No xdg-open for %s", fullpath);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", fullpath, (char*)nullptr);
        _exit(127);
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "Open with default: %s", fullpath);
}

// ---------------------------------------------------------------------------
// Alt+F10 / Ctrl+F8: directory tree window
// ---------------------------------------------------------------------------

// The tree is a flat list of the directories currently visible; expanding a row
// splices its children in below it (TreeRow itself lives in panes.h because
// each Pane owns a model).

// The tree model lives per pane (Pane::tree_rows). The floating Alt+F10 window
// and the embedded panel tree edit the same rows, so the two views agree.

static bool dir_has_children(const char* path) {
    DIR* d = opendir(path);
    if (!d) return false;
    struct dirent* de;
    bool found = false;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[4100];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) { found = true; break; }
    }
    closedir(d);
    return found;
}

// Direct child directories of `path`, sorted, appended to `out` at `depth`.
static void tree_children(const char* path, std::vector<TreeRow>& out, int depth) {
    DIR* d = opendir(path);
    if (!d) return;
    StringList names;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[4100];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) names.push_back(de->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        TreeRow r;
        snprintf(r.path, sizeof(r.path), "%s/%s", path, n.c_str());
        r.depth = depth;
        r.expanded = false;
        r.has_children = dir_has_children(r.path);
        out.push_back(r);
    }
}

// Fill the model with `current_dir` and its child directories.
static void tree_fill(Pane& pane) {
    TreeRows& t = pane.tree_rows;
    t.clear();
    TreeRow root;
    snprintf(root.path, sizeof(root.path), "%s", pane.current_dir);
    root.depth = 0;
    root.expanded = true;
    root.has_children = dir_has_children(root.path);
    t.push_back(root);
    if (root.has_children) tree_children(root.path, t, 1);
    if (pane.tree_selected >= (int)t.size()) pane.tree_selected = 0;
}

static void tree_open(AppPanes* app, Pane& pane) {
    tree_fill(pane);
    app->tree_open = true;
    app->tree_target = (&pane == &app->left) ? 0 : 1;
}

// Ctrl+Shift+F8 (tree instead of the list) / Shift+Tab (tree beside the list).
// The model is refilled on the way in so the tree is always rooted at what the
// pane is showing.
static void tree_mode_toggle(AppPanes* app, Pane& pane, int mode) {
    pane.tree_mode = (pane.tree_mode == mode) ? TREE_OFF : mode;
    if (pane.tree_mode != TREE_OFF) {
        tree_fill(pane);
        pane.tree_selected = 0;
    }
    snprintf(app->status_msg, sizeof(app->status_msg),
             pane.tree_mode == TREE_EMBED ? "Tree panel"
           : pane.tree_mode == TREE_SPLIT ? "Split view: tree + list"
           : "Tree panel off");
}

// Collapse or expand row `idx`.
// Collapse or expand row `idx`. Note the re-indexing after insert/erase: a
// reference into the vector does not survive a reallocation, and writing
// `expanded` through a stale one leaves the row thinking it is still collapsed
// (which then duplicates its children on the next press).
static void tree_toggle(Pane& pane, int idx) {
    TreeRows& t = pane.tree_rows;
    if (idx < 0 || idx >= (int)t.size()) return;
    if (t[idx].expanded) {
        int depth = t[idx].depth;
        int end = idx + 1;
        while (end < (int)t.size() && t[end].depth > depth) end++;
        t.erase(t.begin() + idx + 1, t.begin() + end);
        t[idx].expanded = false;
    } else {
        TreeRows kids;
        tree_children(t[idx].path, kids, t[idx].depth + 1);
        t.insert(t.begin() + idx + 1, kids.begin(), kids.end());
        t[idx].expanded = true;
    }
}

// One pane's worth of tree rows, used by the floating window and by the
// embedded/split panel alike. Clicking a name moves that pane; +/- expands.
static void render_tree_rows(AppPanes* app, Pane& pane, bool focused,
                             float width, float height, const char* id) {
    ImGui::PushID(id);
    ImGui::BeginChild(id, ImVec2(width, height));
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
        app->left_focus = (&pane == &app->left);
    if (pane.tree_rows.empty()) tree_fill(pane);

    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));

    for (int i = 0; i < (int)pane.tree_rows.size(); i++) {
        const TreeRow& row = pane.tree_rows[i];
        const char* base = strrchr(row.path, '/');
        base = (base && base[1]) ? base + 1 : row.path;
        bool sel = focused && (i == pane.tree_selected);
        ImGui::PushID(i);
        ImGui::Indent(row.depth * 12.0f);
        if (row.has_children) {
            if (ImGui::SmallButton(row.expanded ? "-" : "+")) tree_toggle(pane, i);
            ImGui::SameLine();
        }
        if (sel) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetTextLineHeightWithSpacing();
            float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 8.0f;
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(x1, p.y + h),
                                                      mc_theme_colors()->cursor_bg);
        }
        bool clicked = ImGui::Selectable(base, sel);
        if (clicked) {
            pane.tree_selected = i;
            app->left_focus = (&pane == &app->left);
            DIR* probe = opendir(row.path);
            if (probe) { closedir(probe); navigate_to(app, &pane, row.path); }
        }
        ImGui::Unindent(row.depth * 12.0f);
        ImGui::PopID();
    }

    ImGui::PopStyleColor(3);
    ImGui::EndChild();
    ImGui::PopID();
}

// Keyboard for the embedded tree: arrows move, Left/Right collapse and expand,
// Enter sends the pane to the selected directory.
static bool tree_nav(AppPanes* app, Pane& pane, SDL_Keycode key) {
    TreeRows& t = pane.tree_rows;
    if (t.empty()) return false;
    int& sel = pane.tree_selected;
    if (sel < 0) sel = 0;
    if (sel >= (int)t.size()) sel = (int)t.size() - 1;
    switch (key) {
        case SDLK_UP:    if (sel > 0) sel--; return true;
        case SDLK_DOWN:  if (sel + 1 < (int)t.size()) sel++; return true;
        case SDLK_HOME:  sel = 0; return true;
        case SDLK_END:   sel = (int)t.size() - 1; return true;
        case SDLK_PAGEUP: sel = sel > 20 ? sel - 20 : 0; return true;
        case SDLK_PAGEDOWN: sel = (sel + 20 < (int)t.size()) ? sel + 20 : (int)t.size() - 1; return true;
        case SDLK_RIGHT:
            if (!t[sel].expanded && t[sel].has_children) tree_toggle(pane, sel);
            return true;
        case SDLK_LEFT:
            if (t[sel].expanded) {
                tree_toggle(pane, sel);
            } else {
                for (int i = sel - 1; i >= 0; i--)
                    if (t[i].depth == t[sel].depth - 1) { sel = i; break; }
            }
            return true;
        case SDLK_RETURN: case SDLK_KP_ENTER: {
            DIR* probe = opendir(t[sel].path);
            if (probe) { closedir(probe); navigate_to(app, &pane, t[sel].path); }
            return true;
        }
        default: return false;
    }
}

static void render_tree(AppPanes* app) {
    if (!app->tree_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 230, io.DisplaySize.y * 0.5f - 260),
                            ImGuiCond_FirstUseEver);
    bool open = true;
    char title[80];
    snprintf(title, sizeof(title), "Directory tree (Alt+F10) - %s pane",
             app->tree_target == 0 ? "left" : "right");
    ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse);
    ImGui::TextDisabled("click a name to go there, +/- to expand");
    ImGui::Separator();
    Pane& target = app->tree_target == 0 ? app->left : app->right;
    render_tree_rows(app, target, true, 0.0f, ImGui::GetContentRegionAvail().y, "##treefloat");
    if (!open) app->tree_open = false;
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static const char* tab_label(const char* dir, char* buf, size_t n) {
    const char* home = getenv("HOME");
    if (home && strcmp(dir, home) == 0) { snprintf(buf, n, "~"); return buf; }
    const char* base = strrchr(dir, '/');
    snprintf(buf, n, "%s", (base && base[1]) ? base + 1 : dir);
    return buf;
}

// Items shared by the right-click context menu and the F9 pane menu.
static void render_entry_menu_items(AppPanes* app, Pane& pane) {
    FileEntry* e = (pane.selected >= 0 && pane.selected < pane.entry_count)
                       ? pane.entries[pane.selected] : nullptr;
    if (!e || strcmp(e->name, "..") == 0) {
        ImGui::TextDisabled("(no entry under the cursor)");
        return;
    }
    if (ImGui::MenuItem("Open")) activate_entry(app, pane, pane.selected);
    if (ImGui::MenuItem("View")) {
        char full[4100];
        snprintf(full, sizeof(full), "%s/%s", pane.current_dir, entry_rel(pane, pane.selected));
        open_viewer(app, full);
    }
    if (ImGui::MenuItem("Rename")) {
        app->op = OP_RENAME;
        snprintf(app->op_name, sizeof(app->op_name), "%s", e->name);
    }
    bool marked = pane.selected < (int)pane.marked.size() && pane.marked[pane.selected];
    if (ImGui::MenuItem(marked ? "Unmark" : "Mark")) toggle_mark(pane, pane.selected);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy to other pane")) app->pending_op = 1;
    if (ImGui::MenuItem("Move to other pane")) app->pending_op = 2;
    if (ImGui::MenuItem("Paste here", "Ctrl+V")) app->pending_op = 3;
    ImGui::Separator();
    if (ImGui::MenuItem("Delete")) request_delete(app, false);
    if (ImGui::MenuItem(app->trash_mode ? "Deletes go to trash" : "Deletes are always direct"))
        app->trash_mode = app->trash_mode ? 0 : 1;
}

static void render_tab_bar(AppPanes* app, Pane& pane) {
    for (int t = 0; t < pane.tab_count; t++) {
        if (t) ImGui::SameLine();
        char lbl[256];
        tab_label(pane.tabs[t].dir, lbl, sizeof(lbl));
        ImGui::PushID(t);
        if (t == pane.tab_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, mc_theme_colors()->title_focused);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mc_theme_colors()->title_focused_hov);
        }
        if (ImGui::SmallButton(lbl)) tab_switch(&pane, t);
        if (t == pane.tab_active) ImGui::PopStyleColor(2);
        ImGui::PopID();
    }
    if (pane.tab_count < MAX_TABS) {
        ImGui::SameLine();
        ImGui::PushID("tabnew");
        if (ImGui::SmallButton("+")) tab_new(&pane, true);
        ImGui::PopID();
    }
    (void)app;
}

// File list for one pane: view modes (brief/mid/full), context menu,
// double-click to activate.
// Shift+F2 compare: name -> size in the other pane. Cached per pane and only
// rebuilt when that pane's listing changed (list_gen).
static const Pane* cmp_owner[2] = { nullptr, nullptr };
static int         cmp_gen[2]   = { -1, -1 };
static std::map<std::string, off_t> cmp_sizes[2];

static const std::map<std::string, off_t>* compare_map(AppPanes* app, const Pane& other) {
    int slot = (&other == &app->left) ? 0 : 1;
    if (cmp_owner[slot] != &other || cmp_gen[slot] != other.list_gen) {
        cmp_sizes[slot].clear();
        for (int i = 0; i < other.entry_count; i++) {
            const FileEntry* e = other.entries[i];
            if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
            cmp_sizes[slot][e->name] = e->size;
        }
        cmp_owner[slot] = &other;
        cmp_gen[slot] = other.list_gen;
    }
    return &cmp_sizes[slot];
}

// Ctrl+Q: the inactive pane turns into a quick view of the file under the
// cursor in the active pane (TC's quick view panel).
static char qv_buf[128 * 1024];
static char qv_path[4100];

static void render_quick_view_panel(AppPanes* app, float w, float h, const char* id) {
    Pane& src = app->left_focus ? app->left : app->right;
    ImGui::BeginChild(id, ImVec2(w, h));
    if (src.selected < 0 || src.selected >= src.entry_count) {
        ImGui::TextDisabled("(nothing under the cursor)");
        ImGui::EndChild();
        return;
    }
    FileEntry* e = src.entries[src.selected];
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", src.current_dir, entry_rel(src, src.selected));
    if (is_directory(e->name, e->mode)) {
        qv_path[0] = '\0';
        ImGui::TextDisabled("(directory: %s)", e->name);
    } else {
        if (strcmp(full, qv_path) != 0) {
            snprintf(qv_path, sizeof(qv_path), "%s", full);
            long n = fs_read_preview(full, qv_buf, sizeof(qv_buf) - 1);
            if (n < 0) snprintf(qv_buf, sizeof(qv_buf), "(binary or unreadable)");
        }
        ImGui::TextDisabled("%s", e->name);
        ImGui::Separator();
        ImGui::TextUnformatted(qv_buf);
    }
    ImGui::EndChild();
}

// Shift+F1: the long view's column set is user-defined. The spec is a
// comma-separated list; "name" is always present and always first.
enum FileCol { COL_NAME, COL_EXT, COL_SIZE, COL_DATE, COL_PERMS, COL_TYPE, COL_N };
static const char* k_col_key[COL_N]   = { "name", "ext", "size", "date", "perms", "type" };
static const char* k_col_label[COL_N] = { "Name", "Ext", "Size", "Date", "Perms", "Type" };
static const float k_col_width[COL_N] = { 0.0f, 70.0f, 75.0f, 100.0f, 80.0f, 55.0f };

static void columns_parse(const char* spec, bool* on) {
    for (int c = 0; c < COL_N; c++) on[c] = false;
    on[COL_NAME] = true;
    const char* p = spec;
    while (p && *p) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        for (int c = 1; c < COL_N; c++)
            if (len == strlen(k_col_key[c]) && !strncasecmp(p, k_col_key[c], len)) on[c] = true;
        p = comma ? comma + 1 : NULL;
    }
}

// Parsed once per spec change: render_file_list asks for this on every row.
static void columns_spec(const AppPanes* app, bool* on) {
    static char cached[256] = "";
    static bool cached_on[COL_N];
    if (strcmp(cached, app->columns) != 0) {
        columns_parse(app->columns, cached_on);
        snprintf(cached, sizeof(cached), "%s", app->columns);
    }
    for (int c = 0; c < COL_N; c++) on[c] = cached_on[c];
}

static void columns_write(AppPanes* app, const bool* on) {
    char out[256];
    size_t n = (size_t)snprintf(out, sizeof(out), "name");
    for (int c = 1; c < COL_N && n + 16 < sizeof(out); c++)
        if (on[c]) n += (size_t)snprintf(out + n, sizeof(out) - n, ",%s", k_col_key[c]);
    snprintf(app->columns, sizeof(app->columns), "%s", out);
}

static const char* col_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    return (dot && dot != name && dot[1]) ? dot + 1 : "";
}

static const char* col_type(FileEntry* e, bool is_dir, bool is_exec) {
    if (is_dir) return "DIR";
    if (S_ISLNK(e->mode)) return "LNK";
    if (is_exec) return "EXE";
    return "FILE";
}

static void render_columns_dialog(AppPanes* app) {
    if (!app->columns_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(300, 260), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 150, io.DisplaySize.y * 0.5f - 130), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Define columns", &open, ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("Columns of the long view (Ctrl+F2).\nName is always the first column.");
    ImGui::Separator();
    bool on[COL_N];
    columns_spec(app, on);
    bool changed = false;
    for (int c = 1; c < COL_N; c++) {
        if (ImGui::Checkbox(k_col_label[c], &on[c])) changed = true;
    }
    if (changed) columns_write(app, on);
    ImGui::Separator();
    if (ImGui::Button("Short")) snprintf(app->columns, sizeof(app->columns), "name,size");
    ImGui::SameLine();
    if (ImGui::Button("Long")) snprintf(app->columns, sizeof(app->columns), "name,size,date,perms");
    ImGui::SameLine();
    if (ImGui::Button("All")) snprintf(app->columns, sizeof(app->columns), "name,ext,size,date,perms,type");
    ImGui::Spacing();
    ImGui::TextDisabled("Current: %s", app->columns);
    if (!open) app->columns_open = false;
    ImGui::End();
}

// The rows of one pane, called with its child window already open. The wrapper
// at the end decides whether that child holds the list, a tree, or both.
static void render_file_list_body(AppPanes* app, Pane& pane, bool focused) {
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    pane.page_size = row_h > 0 ? (int)(ImGui::GetWindowSize().y / row_h) : PAGE_SIZE;
    if (pane.page_size < 1) pane.page_size = PAGE_SIZE;

    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
        app->left_focus = focused;

    // The filter field is edited by ImGui, so re-apply the filter here, before
    // the rows are walked.
    if (strcmp(pane.filter, pane.filter_applied) != 0) rebuild_view(pane);

    // Hover/click must not change row colors. Selection and marking backgrounds
    // are drawn explicitly (below) so they survive the mouse hovering the row.
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));

    bool use_table = (app->view_mode == VIEW_FULL || app->view_mode == VIEW_MID);

    // Shift+F2: rows are colored by how they differ from the other pane.
    const std::map<std::string, off_t>* cmp = nullptr;
    if (app->compare_on)
        cmp = compare_map(app, (&pane == &app->left) ? app->right : app->left);

    if (use_table) {
        // The long view follows the Shift+F1 column set; the mid view keeps the
        // fixed name/size/date trio.
        bool col_on[COL_N];
        int col_order[COL_N], ncols = 0;
        if (app->view_mode == VIEW_FULL) {
            columns_spec(app, col_on);
            for (int c = 0; c < COL_N; c++) if (col_on[c]) col_order[ncols++] = c;
        } else {
            col_order[ncols++] = COL_NAME;
            col_order[ncols++] = COL_SIZE;
            col_order[ncols++] = COL_DATE;
        }
        if (ImGui::BeginTable("##filetable", ncols,
                              ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingStretchProp)) {
            for (int k = 0; k < ncols; k++) {
                int c = col_order[k];
                if (c == COL_NAME)
                    ImGui::TableSetupColumn(k_col_label[c], ImGuiTableColumnFlags_WidthStretch, 0.42f);
                else
                    ImGui::TableSetupColumn(k_col_label[c], ImGuiTableColumnFlags_WidthFixed, k_col_width[c]);
            }

            for (int r = 0; r < (int)pane.view.size(); r++) {
                int i = pane.view[r];
                FileEntry* entry = pane.entries[i];
                bool selected = focused && (i == pane.selected);
                bool is_marked = i < (int)pane.marked.size() && pane.marked[i];
                bool is_dir = is_directory(entry->name, entry->mode);
                bool is_exec = is_executable(entry->name, entry->mode);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                int pushed = 0;
                if (is_marked) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    pushed++;
                }
                if (is_dir) {
                    ImGui::PushStyleColor(ImGuiCol_Text, mc_theme_colors()->dir);
                    pushed++;
                } else if (is_exec) {
                    ImGui::PushStyleColor(ImGuiCol_Text, mc_theme_colors()->exec);
                    pushed++;
                }
                if (cmp && strcmp(entry->name, "..") != 0) {
                    std::map<std::string, off_t>::const_iterator it = cmp->find(entry->name);
                    if (it == cmp->end()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, mc_theme_colors()->diff_missing);
                        pushed++;
                    } else if (it->second != entry->size) {
                        ImGui::PushStyleColor(ImGuiCol_Text, mc_theme_colors()->diff_differ);
                        pushed++;
                    }
                }

                // Explicit row background: green = marked, blue = cursor.
                // Marked + cursor: green fill with a blue outline so the cursor
                // row stays visible on top of a marked file.
                {
                    ImU32 bg = 0;
                    if (is_marked)      bg = mc_theme_colors()->marked_bg;
                    else if (selected)  bg = mc_theme_colors()->cursor_bg;
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float h = ImGui::GetTextLineHeightWithSpacing();
                    float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 8.0f;
                    if (bg)
                        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(x1, p.y + h), bg);
                    if (is_marked && selected)
                        ImGui::GetWindowDrawList()->AddRect(p, ImVec2(x1, p.y + h),
                                                            mc_theme_colors()->cursor_outline, 0.0f, 2.0f);
                }

                bool clicked = ImGui::Selectable(entry_rel(pane, i), selected, ImGuiSelectableFlags_SpanAllColumns);
                if (selected) {
                    ImVec2 rp = ImGui::GetCursorScreenPos();
                    pane.cursor_screen_x = rp.x;
                    pane.cursor_screen_y = rp.y;
                }
                bool dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                bool rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);

                if (selected && pane.scroll_request) {
                    ImGui::SetScrollY(ImGui::GetCursorPosY() - ImGui::GetWindowSize().y * 0.5f);
                    pane.scroll_request = false;
                }
                if (rclick) {
                    pane.selected = i;
                    app->left_focus = focused;
                    ImGui::OpenPopup("ctxmenu");
                }
                if (clicked) {
                    pane.selected = i;
                    app->left_focus = focused;
                    if (ImGui::GetIO().KeyShift) mark_range(pane, pane.anchor, i);
                    else pane.anchor = i;
                }
                if (dbl) activate_entry(app, pane, i);

                if (pushed) ImGui::PopStyleColor(pushed);

                char sz[32], dt[32];
                if (is_dir) {
                    // TC shows a directory's summed size once it has been computed
                    // (Space, or Alt+Shift+Enter for all of them).
                    if (i < (int)pane.dir_size.size() && pane.dir_size[i] >= 0)
                        human_size((off_t)pane.dir_size[i], sz, sizeof(sz));
                    else
                        snprintf(sz, sizeof(sz), "-");
                } else {
                    human_size(entry->size, sz, sizeof(sz));
                }
                fmt_date(entry->mtime, dt, sizeof(dt));
                char pm[16];
                format_mode(entry->mode, pm);
                for (int k = 1; k < ncols; k++) {
                    ImGui::TableSetColumnIndex(k);
                    switch (col_order[k]) {
                        case COL_EXT:   ImGui::TextUnformatted(col_ext(entry_rel(pane, i))); break;
                        case COL_SIZE:  ImGui::TextUnformatted(sz); break;
                        case COL_DATE:  ImGui::TextUnformatted(dt); break;
                        case COL_PERMS: ImGui::TextUnformatted(pm); break;
                        case COL_TYPE:  ImGui::TextUnformatted(col_type(entry, is_dir, is_exec)); break;
                        default: break;
                    }
                }
            }
            ImGui::EndTable();
        }
    } else {
        // BRIEF: two name-only columns
        ImGui::Columns(2, "briefcols", false);
        for (int r = 0; r < (int)pane.view.size(); r++) {
            int i = pane.view[r];
            FileEntry* entry = pane.entries[i];
            bool selected = focused && (i == pane.selected);
            bool is_marked = i < (int)pane.marked.size() && pane.marked[i];
            bool is_dir = is_directory(entry->name, entry->mode);
            bool is_exec = is_executable(entry->name, entry->mode);

            int pushed = 0;
            if (is_marked) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                pushed++;
            }
            if (is_dir) {
                ImGui::PushStyleColor(ImGuiCol_Text, mc_theme_colors()->dir);
                pushed++;
            } else if (is_exec) {
                ImGui::PushStyleColor(ImGuiCol_Text, mc_theme_colors()->exec);
                pushed++;
            }
            if (cmp && strcmp(entry->name, "..") != 0) {
                std::map<std::string, off_t>::const_iterator it = cmp->find(entry->name);
                if (it == cmp->end()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, mc_theme_colors()->diff_missing);
                    pushed++;
                } else if (it->second != entry->size) {
                    ImGui::PushStyleColor(ImGuiCol_Text, mc_theme_colors()->diff_differ);
                    pushed++;
                }
            }

            // Explicit row background: green = marked, blue = cursor.
            // Marked + cursor: green fill with a blue outline so the cursor
            // row stays visible on top of a marked file.
            {
                ImU32 bg = 0;
                if (is_marked)      bg = mc_theme_colors()->marked_bg;
                else if (selected)  bg = mc_theme_colors()->cursor_bg;
                ImVec2 p = ImGui::GetCursorScreenPos();
                float h = ImGui::GetTextLineHeightWithSpacing();
                float w = ImGui::GetContentRegionAvail().x;
                if (bg)
                    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg);
                if (is_marked && selected)
                    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + w, p.y + h),
                                                        mc_theme_colors()->cursor_outline, 0.0f, 2.0f);
            }

            bool clicked = ImGui::Selectable(entry_rel(pane, i), selected);
            if (selected) {
                ImVec2 rp = ImGui::GetCursorScreenPos();
                pane.cursor_screen_x = rp.x;
                pane.cursor_screen_y = rp.y;
            }
            bool dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            bool rclick = ImGui::IsItemClicked(ImGuiMouseButton_Right);

            if (selected && pane.scroll_request) {
                ImGui::SetScrollY(ImGui::GetCursorPosY() - ImGui::GetWindowSize().y * 0.5f);
                pane.scroll_request = false;
            }
            if (rclick) {
                pane.selected = i;
                app->left_focus = focused;
                ImGui::OpenPopup("ctxmenu");
            }
            if (clicked) {
                pane.selected = i;
                app->left_focus = focused;
                if (ImGui::GetIO().KeyShift) mark_range(pane, pane.anchor, i);
                else pane.anchor = i;
            }
            if (dbl) activate_entry(app, pane, i);

            if (pushed) ImGui::PopStyleColor(pushed);
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
    }

    if (pane.view.empty())
        ImGui::TextDisabled("(no entries match '%s')", pane.filter);

    ImGui::PopStyleColor(3); // Header, HeaderHovered, HeaderActive

    // F9 pane menu (TC: "menu above the source directory"). Opened from the
    // keyboard at the top of the focused pane; dismissed by Esc or click-away.
    if (focused && app->pane_menu_open) {
        ImGui::OpenPopup("panemenu");
        app->pane_menu_open = false;
    }
    if (ImGui::BeginPopup("panemenu")) {
        app->pane_menu_alive = true;
        if (app->popup_close_request) ImGui::CloseCurrentPopup();
        render_entry_menu_items(app, pane);
        ImGui::EndPopup();
    }

    // Shift+F10: the local menu at the cursor row, like a right click.
    if (focused && app->entry_menu_open) {
        ImGui::SetNextWindowPos(ImVec2(pane.cursor_screen_x, pane.cursor_screen_y));
        ImGui::OpenPopup("ctxmenu");
        app->entry_menu_open = false;
    }

    // Right-click context menu
    if (ImGui::BeginPopup("ctxmenu")) {
        app->ctx_menu_alive = true;
        if (app->popup_close_request) ImGui::CloseCurrentPopup();
        render_entry_menu_items(app, pane);
        ImGui::EndPopup();
    }
}

// The content area of a pane: the file list, the TC tree panel (Ctrl+Shift+F8),
// or a tree + list split (Shift+Tab). The child window opened here is what the
// body above draws into.
static void render_file_list(AppPanes* app, Pane& pane, bool focused, float width, float height, const char* id) {
    if (pane.tree_mode == TREE_EMBED) {
        render_tree_rows(app, pane, focused, width, height, id);
        return;
    }
    if (pane.tree_mode == TREE_SPLIT) {
        float tw = width * 0.40f;
        float gap = ImGui::GetStyle().ItemSpacing.x;
        ImGui::PushID(id);
        render_tree_rows(app, pane, focused, tw, height, "##treehalf");
        ImGui::SameLine();
        ImGui::BeginChild("##listhalf", ImVec2(width - tw - gap, height));
        render_file_list_body(app, pane, focused);
        ImGui::EndChild();
        ImGui::PopID();
        return;
    }
    ImGui::PushID(id);
    ImGui::BeginChild(id, ImVec2(width, height));
    render_file_list_body(app, pane, focused);
    ImGui::EndChild();
    ImGui::PopID();
}

static void render_quick_view(AppPanes* app, Pane& pane) {
    ImGui::SetNextWindowSize(ImVec2(720, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 640, 60), ImGuiCond_FirstUseEver);
    ImGui::Begin("Quick view");

    if (pane.selected < 0 || pane.selected >= pane.entry_count) {
        ImGui::TextUnformatted("(empty)");
        ImGui::End();
        return;
    }
    FileEntry* e = pane.entries[pane.selected];

    char mode[16];
    format_mode(e->mode, mode);
    ImGui::Text("%s", e->name);
    ImGui::Text("Type: %s", is_directory(e->name, e->mode) ? "directory"
                 : is_executable(e->name, e->mode) ? "executable" : "file");
    ImGui::Text("Size: %lld", (long long)e->size);
    ImGui::Text("Mode: %s", mode);
    ImGui::Separator();

    if (!is_directory(e->name, e->mode)) {
        char full[4100];
        snprintf(full, sizeof(full), "%s/%s", pane.current_dir, entry_rel(pane, pane.selected));
        static char preview[8192];
        long n = fs_read_preview(full, preview, sizeof(preview) - 1);
        if (n < 0) {
            static char hexbuf[16384];
            long dumped = fs_hex_dump(full, hexbuf, sizeof(hexbuf), 2048);
            if (dumped < 0) ImGui::TextUnformatted("(unreadable)");
            else {
                ImGui::TextUnformatted("(binary - hex dump)");
                ImGui::TextUnformatted(hexbuf);
            }
        } else {
            ImGui::TextUnformatted(preview);
        }
    }
    ImGui::End();
}

static void render_viewer(AppPanes* app) {
    if (!app->viewer_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.7f, io.DisplaySize.y * 0.8f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.15f, io.DisplaySize.y * 0.1f), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("File viewer", &open, ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted(app->viewer_path);
    ImGui::Separator();
    ImGui::BeginChild("##viewerbody", ImVec2(0, 0));
    ImGui::TextUnformatted(viewer_buf);
    if (viewer_dirty) {
        float maxs = ImGui::GetScrollMaxY();
        if (viewer_scroll > maxs) viewer_scroll = maxs;
        if (viewer_scroll < 0) viewer_scroll = 0;
        ImGui::SetScrollY(viewer_scroll);
        viewer_dirty = false;
    }
    ImGui::EndChild();
    if (!open) app->viewer_open = false;
    ImGui::End();
}

static void render_output(AppPanes* app) {
    if (!app->output_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.6f, io.DisplaySize.y * 0.6f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.2f, io.DisplaySize.y * 0.2f), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Command output", &open, ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted(app->output_text);
    if (!open) app->output_open = false;
    ImGui::End();
}

static void render_hotlist(AppPanes* app) {
    if (!app->hotlist_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 280, io.DisplaySize.y * 0.5f - 210), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Hotlist (Alt+D adds current dir)", &open, ImGuiWindowFlags_NoCollapse);
    if (app->hotlist.empty())
        ImGui::TextUnformatted("(empty - Alt+D adds the current directory)");
    for (int i = 0; i < (int)app->hotlist.size(); i++) {
        const std::string& dir = app->hotlist[i];
        ImGui::PushID(i);
        if (ImGui::Selectable(dir.c_str(), false)) {
            Pane& active = app->left_focus ? app->left : app->right;
            DIR* probe = opendir(dir.c_str());
            if (probe) { closedir(probe); navigate_to(app, &active, dir.c_str()); }
            app->hotlist_open = false;
            ImGui::PopID();
            break;
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - 70);
        if (ImGui::SmallButton("remove")) {
            app->hotlist.erase(app->hotlist.begin() + i);
            hotlist_save(app);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (!open) app->hotlist_open = false;
    ImGui::End();
}

// Alt+Down: the directories this pane has already visited, newest first.
static void render_dirhist(AppPanes* app) {
    if (!app->dirhist_open) return;
    Pane& active = app->left_focus ? app->left : app->right;
    ImGui::SetNextWindowSize(ImVec2(560, 380), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Visited directories (Alt+Down)", &open, ImGuiWindowFlags_NoCollapse);
    if (active.history_count == 0)
        ImGui::TextUnformatted("(empty - navigate around first)");
    for (int k = active.history_count - 1; k >= 0; k--) {
        ImGui::PushID(k);
        if (ImGui::Selectable(active.history[k], k == active.history_pos)) {
            if (k != active.history_pos) {
                remember_selection(&active);
                active.history_pos = k;
                refresh_pane(&active, active.history[k]);
                restore_selection(&active);
                if (active.tab_count > 0)
                    snprintf(active.tabs[active.tab_active].dir,
                             sizeof(active.tabs[0].dir), "%s", active.current_dir);
            }
            app->dirhist_open = false;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (!open) app->dirhist_open = false;
    ImGui::End();
}

// Alt+F8: the command line history as a list (TC pops it up above the prompt).
static void render_cmdhist(AppPanes* app) {
    if (!app->cmdhist_open) return;
    ImGui::SetNextWindowSize(ImVec2(680, 300), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Command history (Alt+F8)", &open, ImGuiWindowFlags_NoCollapse);
    if (app->cmd_hist_count == 0) ImGui::TextUnformatted("(empty)");
    for (int i = app->cmd_hist_count - 1; i >= 0; i--) {
        ImGui::PushID(i);
        if (ImGui::Selectable(app->cmd_history[i], false)) {
            snprintf(app->cmd, sizeof(app->cmd), "%s", app->cmd_history[i]);
            app->cmd_hist_pos = app->cmd_hist_count;
            app->cmdhist_open = false;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (!open) app->cmdhist_open = false;
    ImGui::End();
}

// Mounted drives for the Alt+F1/F2 picker. /proc/mounts holds
// "device mountpoint fstype options dump pass"; pseudo filesystems are skipped.
static void list_mounts(StringList* mnts, StringList* labels) {
    FILE* f = fopen("/proc/mounts", "r");
    if (!f) return;
    static const char* skip[] = { "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "squashfs",
                                  "autofs", "cgroup", "cgroup2", "mqueue", "overlay", "fusectl",
                                  "configfs", "binfmt_misc", "tracefs", "debugfs", "securityfs",
                                  "efivarfs", "pstore", "rpc_pipefs", "nsfs", "ramfs", "bpf",
                                  "hugetlbfs", "none" };
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char dev[256], mnt[512], fstype[128];
        if (sscanf(line, "%255s %511s %127s", dev, mnt, fstype) != 3) continue;
        bool skip_it = !strncmp(mnt, "/proc", 5) || !strncmp(mnt, "/sys", 4) ||
                       !strncmp(mnt, "/dev", 4) || !strncmp(mnt, "/run", 4);
        for (const char* s : skip) if (!strcmp(fstype, s)) { skip_it = true; break; }
        if (!strncmp(fstype, "fuse.", 5)) skip_it = true;
        if (skip_it) continue;
        char label[920];
        snprintf(label, sizeof(label), "%-32s %s (%s)", mnt, dev, fstype);
        mnts->push_back(mnt);
        labels->push_back(label);
    }
    fclose(f);
}

// Alt+F1 / Alt+F2: choose the drive shown in the left/right pane.
static void render_volumes(AppPanes* app) {
    if (!app->volumes_open) return;
    StringList mnt, label;
    list_mounts(&mnt, &label);
    ImGui::SetNextWindowSize(ImVec2(640, 320), ImGuiCond_FirstUseEver);
    bool open = true;
    char title[80];
    snprintf(title, sizeof(title), "Drives for the %s pane (Alt+F1/F2)",
             app->volumes_target == 0 ? "left" : "right");
    ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse);
    if (mnt.empty()) ImGui::TextUnformatted("(no mounted drives found)");
    for (int i = 0; i < (int)mnt.size(); i++) {
        ImGui::PushID(i);
        if (ImGui::Selectable(label[i].c_str(), false)) {
            Pane& pane = app->volumes_target == 0 ? app->left : app->right;
            DIR* probe = opendir(mnt[i].c_str());
            if (probe) {
                closedir(probe);
                navigate_to(app, &pane, mnt[i].c_str());
                app->left_focus = app->volumes_target == 0;
            } else {
                snprintf(app->status_msg, sizeof(app->status_msg), "Cannot open %s", mnt[i].c_str());
            }
            app->volumes_open = false;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (!open) app->volumes_open = false;
    ImGui::End();
}

static void render_search(AppPanes* app) {
    if (!app->search_open) { app->search_input_active = false; return; }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 310, io.DisplaySize.y * 0.5f - 230), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Search files", &open, ImGuiWindowFlags_NoCollapse);

    ImGui::SetNextItemWidth(-90);
    if (ImGui::InputText("##pattern", app->search_pattern, sizeof(app->search_pattern)))
        run_search(app);   // Enter (or focus loss) runs the search
    app->search_input_active = ImGui::IsItemActive();
    ImGui::SameLine();
    if (ImGui::Button("Search")) run_search(app);

    ImGui::Text("%zu result%s%s", app->search_results.size(),
                app->search_results.size() == 1 ? "" : "s",
                app->search_results.size() >= 1000 ? " (capped at 1000)" : "");
    ImGui::Separator();

    ImGui::BeginChild("##results", ImVec2(0, 0));
    for (size_t i = 0; i < app->search_results.size(); i++) {
        const std::string& full = app->search_results[i];
        ImGui::PushID((int)i);
        if (ImGui::Selectable(full.c_str(), false)) {
            char dir[4100];
            snprintf(dir, sizeof(dir), "%s", full.c_str());
            char* slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
            const char* base = slash ? slash + 1 : full.c_str();
            Pane& active = app->left_focus ? app->left : app->right;
            DIR* probe = opendir(dir);
            if (probe) {
                closedir(probe);
                navigate_to(app, &active, dir);
                for (int j = 0; j < active.entry_count; j++) {
                    if (strcmp(active.entries[j]->name, base) == 0) { active.selected = j; break; }
                }
                active.scroll_request = true;
            }
            app->search_open = false;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (!open) app->search_open = false;
    ImGui::End();
}

static void render_confirm(AppPanes* app) {
    if (app->confirm == CONF_NONE) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(520, 130), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 260, io.DisplaySize.y * 0.5f - 65), ImGuiCond_Always);
    ImGui::Begin("Confirm", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
    ImGui::TextWrapped("%s", app->confirm_text);
    ImGui::Spacing();
    if (ImGui::Button("Yes")) app->pending_confirm_yes = true;
    ImGui::SameLine();
    if (ImGui::Button("No")) { app->confirm = CONF_NONE; app->confirm_text[0] = '\0'; }
    ImGui::SameLine();
    ImGui::TextUnformatted("(press Y / N)");
    ImGui::End();
}

static void render_about(AppPanes* app) {
    if (!app->about_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(600, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 300, io.DisplaySize.y * 0.5f - 270), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("About mc-clone", &open, ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted(
        "mc-clone - a Total Commander style file manager (Dear ImGui + SDL3)\n\n"
        "TOTAL COMMANDER KEYS\n"
        "F1 help            F9 pane menu     F10 app menu    Alt+F4 quit\n"
        "F3 view            F4 edit          F5 copy         F6 move/rename\n"
        "F7 new folder      F8 / Del to the trash   Shift+F8 / Shift+Del direct delete\n"
        "Ins / Space        mark             Num+ / Num- extend / shrink\n"
        "Num* invert        Num/ restore     Ctrl+Num+ all   Ctrl+Num- none\n"
        "Alt+Num+/- mark or unmark the same extension\n"
        "Ctrl+A all         Ctrl+I invert    Shift+arrows range mark\n"
        "Ctrl+F3/F4/F5/F6/F7 sort name / ext / date / size / unsorted\n"
        "Shift+F3 view only the entry under the cursor\n"
        "Tab switch pane    Enter open/run   Backspace or Ctrl+PgUp parent\n"
        "Ctrl+PgDn open entry under cursor  Alt+Left/Right dir history\n"
        "Ctrl+T new tab     Ctrl+W close     Ctrl+Shift+W close all tabs\n"
        "Ctrl+Tab / Ctrl+Shift+Tab cycle    Ctrl+1..9 jump to a tab\n"
        "Ctrl+Up / Ctrl+Shift+Down open the cursor dir in a new tab\n"
        "Ctrl+Left/Right show the cursor dir in the other pane\n"
        "Ctrl+U swap the pane directories   Ctrl+, go to the root\n"
        "Ctrl+C copy        Ctrl+X cut       Ctrl+V paste\n"
        "Alt+Down dir history   Alt+F8 command history   Alt+F1/F2 choose drive\n"
        "Alt+F11/F12 keyboard-focus the left / right path bar\n"
        "Alt+Enter properties               Ctrl+R refresh     Ctrl+O view mode\n"
        "Alt+. hidden files Alt+D add to hotlist  Shift+F9 hotlist  Alt+F7 search\n"
        "Ctrl+S quick filter  Ctrl+Shift+S re-apply  Ctrl+F10 clear filter\n"
        "Ctrl+F11 executables only  Ctrl+F12 filter preset from the config\n"
        "Shift+F10 / Menu local menu   Shift+Esc minimize window   F1 hides this help\n"
        "Ctrl+F1/F2 short / long view  Shift+F2 compare panes  Ctrl+Q quick view panel\n"
        "Alt+F10 or Ctrl+F8 dir tree   Ctrl+B branch view (Ctrl+Shift+B marked only)\n"
        "Alt+F3 open with the default application\n"
        "Shift+F4 new file and edit    Shift+F5 copy in place  Shift+F6 rename in place\n"
        "Shift+Ctrl+F5 link in the other pane  Ctrl+L count marked  Alt+Shift+Enter dir sizes\n"
        "AltGr + letter quick search (Esc ends it)   Shift+F1 define the long view columns\n"
        "Ctrl+Shift+F8 tree in the panel   Shift+Tab split view: tree + list\n\n"
        "$ command line     type a command, Enter runs it (cd, exit builtins)\n"
        "Alt+C prefill 'cd '   Alt+PgUp/PgDn command history   Esc clear\n");
    if (!open) app->about_open = false;
    ImGui::End();
}

// Properties dialog (Alt+Enter): recursive count + size of the selected entry.
static void open_properties(AppPanes* app, Pane& pane) {
    if (pane.selected < 0 || pane.selected >= pane.entry_count) return;
    FileEntry* e = pane.entries[pane.selected];
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", pane.current_dir, entry_rel(pane, pane.selected));
    snprintf(app->props_path, sizeof(app->props_path), "%s", full);
    fs_count_tree(full, &app->props_size, &app->props_files, &app->props_dirs);
    app->props_open = true;
}

static void render_properties(AppPanes* app) {
    if (!app->props_open) return;
    ImGui::SetNextWindowSize(ImVec2(460, 200), ImGuiCond_FirstUseEver);
    ImGui::Begin("Properties", &app->props_open);
    char sz[64];
    human_size((off_t)app->props_size, sz, sizeof(sz));
    ImGui::TextUnformatted(app->props_path);
    ImGui::Separator();
    ImGui::Text("Total size: %s (%lld bytes)", sz, app->props_size);
    ImGui::Text("Files:       %ld", app->props_files);
    if (app->props_dirs > 0)
        ImGui::Text("Directories: %ld", app->props_dirs);
    ImGui::Spacing();
    if (ImGui::Button("Close")) app->props_open = false;
    ImGui::End();
}

// TC quick filter row, drawn between the path bar and the list. ImGui owns
// the text; render_file_list notices the change and rebuilds the view.
static void render_filter_row(AppPanes* app, Pane& pane, const char* id) {
    if (!pane.filter_active && pane.filter[0] == '\0' && !pane.filter_exec_only) return;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, mc_theme_colors()->title_unfocused);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, mc_theme_colors()->title_unfocused);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (pane.filter_focus_request) {
        ImGui::SetKeyboardFocusHere(-1);
        pane.filter_focus_request = false;
    }
    ImGui::PushID(id);
    ImGui::InputText("##filter", pane.filter, sizeof(pane.filter));
    if (ImGui::IsItemClicked()) app->left_focus = (&pane == &app->left);
    pane.filter_field_active = ImGui::IsItemActive();
    ImGui::PopID();
    ImGui::PopStyleColor(2);
}

// Commands menu items. Shared by the menu bar and the F10 keyboard menu so the
// two can never drift apart.
// Defined further down with the keyboard handlers; the menu offers them too.
static void do_copy_in_place(AppPanes* app);
static void do_symlink_other(AppPanes* app);
static void count_marked_sizes(AppPanes* app);
static void fill_dir_sizes(AppPanes* app);

static void render_commands_items(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other  = app->left_focus ? app->right : app->left;

    if (ImGui::MenuItem("Quick view", "F3")) active.quick_view = !active.quick_view;
    if (ImGui::MenuItem("Quick filter", "Ctrl+S")) {
        active.filter_active = true;
        active.filter_focus_request = true;
    }
    if (ImGui::MenuItem("Re-apply last filter", "Ctrl+Shift+S") && active.last_filter[0]) {
        snprintf(active.filter, sizeof(active.filter), "%s", active.last_filter);
        active.filter_active = true;
    }
    if (ImGui::MenuItem("Clear filter", "Ctrl+F10")) {
        active.filter[0] = '\0';
        active.filter_exec_only = false;
        active.filter_active = false;
    }
    if (ImGui::MenuItem("Executables only", "Ctrl+F11")) {
        active.filter_exec_only = !active.filter_exec_only;
        active.filter_active = active.filter_exec_only || active.filter[0] != '\0';
    }
    if (ImGui::MenuItem("Apply filter preset", "Ctrl+F12") && app->filter_preset[0]) {
        snprintf(active.filter, sizeof(active.filter), "%s", app->filter_preset);
        active.filter_active = true;
    }
    if (ImGui::MenuItem("Search files", "Alt+F7")) app->search_open = !app->search_open;
    if (ImGui::MenuItem("Hotlist", "Shift+F9")) app->hotlist_open = !app->hotlist_open;
    if (ImGui::MenuItem("Visited directories", "Alt+Down")) app->dirhist_open = !app->dirhist_open;
    if (ImGui::MenuItem("Command history", "Alt+F8")) app->cmdhist_open = !app->cmdhist_open;
    if (ImGui::MenuItem("Left drive", "Alt+F1")) { app->volumes_open = true; app->volumes_target = 0; }
    if (ImGui::MenuItem("Right drive", "Alt+F2")) { app->volumes_open = true; app->volumes_target = 1; }
    if (ImGui::MenuItem("Directory tree", "Alt+F10")) {
        if (app->tree_open) app->tree_open = false; else tree_open(app, active);
    }
    if (ImGui::MenuItem("Tree in this panel", "Ctrl+Shift+F8")) tree_mode_toggle(app, active, TREE_EMBED);
    if (ImGui::MenuItem("Split view: tree + list", "Shift+Tab")) tree_mode_toggle(app, active, TREE_SPLIT);
    if (ImGui::MenuItem("Define columns", "Shift+F1")) app->columns_open = !app->columns_open;
    // Reserved but not implemented: greyed out so the shortcut is discoverable
    // without pretending the feature exists.
    ImGui::MenuItem("Pack archive", "Alt+F5", false, false);
    ImGui::MenuItem("Extract archive", "Alt+F6", false, false);
    if (ImGui::MenuItem("Properties", "Alt+Enter")) open_properties(app, active);
    if (ImGui::MenuItem("New file", "Shift+F4")) { app->op = OP_NEWFILE; app->op_name[0] = '\0'; }
    if (ImGui::MenuItem("Copy in place", "Shift+F5")) do_copy_in_place(app);
    if (ImGui::MenuItem("Link in other pane", "Shift+Ctrl+F5")) do_symlink_other(app);
    if (ImGui::MenuItem("Count marked", "Ctrl+L")) count_marked_sizes(app);
    if (ImGui::MenuItem("Fill directory sizes", "Alt+Shift+Enter")) fill_dir_sizes(app);
    if (ImGui::MenuItem("Add to hotlist", "Alt+D")) {
        bool dup = false;
        for (const std::string& s : app->hotlist) if (s == active.current_dir) { dup = true; break; }
        if (!dup) { app->hotlist.push_back(active.current_dir); hotlist_save(app); }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy", "Ctrl+C")) clipboard_set(app, active, false);
    if (ImGui::MenuItem("Cut", "Ctrl+X")) clipboard_set(app, active, true);
    // Paste pumps its own progress frames, so it has to run between frames.
    if (ImGui::MenuItem("Paste", "Ctrl+V")) app->pending_op = 3;
    ImGui::Separator();
    if (ImGui::MenuItem("Mark all", "Ctrl+Num+")) select_all_marks(active);
    if (ImGui::MenuItem("Clear marks", "Ctrl+Num-")) clear_marks(active);
    if (ImGui::MenuItem("Invert marks", "Ctrl+I")) invert_marks(active);
    if (ImGui::MenuItem("Swap panes", "Ctrl+U")) swap_panes(app, false);
    if (ImGui::MenuItem("Branch view", "Ctrl+B")) branch_toggle(app, active, false);
    if (ImGui::MenuItem("Branch marked dirs", "Ctrl+Shift+B")) branch_toggle(app, active, true);
    if (ImGui::MenuItem("Go to root", "Ctrl+,")) navigate_to(app, &active, "/");
    if (ImGui::MenuItem("Other pane to cursor dir", "Ctrl+Left")) open_cursor_in_other_pane(app, &active, &other);
    ImGui::Separator();
    if (ImGui::MenuItem("Cycle view mode", "Ctrl+O"))
        app->view_mode = (app->view_mode + 1) % 3;
    if (ImGui::MenuItem("View short", "Ctrl+F1")) app->view_mode = VIEW_BRIEF;
    if (ImGui::MenuItem("View long", "Ctrl+F2")) app->view_mode = VIEW_FULL;
    if (ImGui::MenuItem("Compare panes", "Shift+F2")) app->compare_on = !app->compare_on;
    if (ImGui::MenuItem("Quick view panel", "Ctrl+Q")) app->quick_view_pane = !app->quick_view_pane;
    static const int sort_keys[5] = { FS_SORT_NAME, FS_SORT_EXT, FS_SORT_DATE, FS_SORT_SIZE, FS_SORT_NONE };
    static const char* sort_names[5] = { "Sort by name", "Sort by extension", "Sort by date",
                                         "Sort by size", "Unsorted" };
    static const char* sort_short[5] = { "name", "ext", "date", "size", "unsorted" };
    static const char* sort_hints[5] = { "Ctrl+F3", "Ctrl+F4", "Ctrl+F5", "Ctrl+F6", "Ctrl+F7" };
    for (int i = 0; i < 5; i++) {
        if (ImGui::MenuItem(sort_names[i], sort_hints[i])) {
            fs_set_sort_mode(sort_keys[i]);
            snprintf(app->status_msg, sizeof(app->status_msg), "Sort by: %s", sort_short[i]);
            app_panes_refresh_left(app);
            app_panes_refresh_right(app);
        }
    }
    if (ImGui::MenuItem("Show hidden files", "Alt+.")) {
        app->show_hidden = !app->show_hidden;
        fs_set_show_hidden(app->show_hidden);
        app_panes_refresh_left(app);
        app_panes_refresh_right(app);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("New tab", "Ctrl+T")) tab_new(&active, true);
    if (ImGui::MenuItem("New tab, stay here", "Ctrl+Shift+T")) tab_new(&active, false);
    if (ImGui::MenuItem("Close tab", "Ctrl+W")) tab_close(&active);
    if (ImGui::MenuItem("Close all tabs", "Ctrl+Shift+W")) tab_close_all(&active);
    if (ImGui::MenuItem("Next tab", "Ctrl+Tab")) tab_cycle(&active, 1);
    if (ImGui::MenuItem("Previous tab", "Ctrl+Shift+Tab")) tab_cycle(&active, -1);
    ImGui::Separator();
    if (ImGui::MenuItem("Refresh", "Ctrl+R"))
        app->left_focus ? app_panes_refresh_left(app) : app_panes_refresh_right(app);
    if (ImGui::MenuItem("Quit", "Alt+F4")) app->quit_request = true;
}

void app_panes_render(AppPanes* app) {
    ImGuiIO& io = ImGui::GetIO();
    bool& left_focus = app->left_focus;
    Pane& left = app->left;
    Pane& right = app->right;

    // Fullscreen main window (MC-style, no decorations)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Midnight Commander", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollbar);

    // Top menu bar. The Commands items also back the F10 keyboard menu.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Commands")) {
            render_commands_items(app);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About / keys", "F1")) app->about_open = !app->about_open;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // F10 app menu: opened from the keyboard, dismissed by Esc or click-away.
    // The *_alive flags tell the key handler whether a menu popup is up.
    app->menu_alive = false;
    app->pane_menu_alive = false;
    app->ctx_menu_alive = false;
    if (app->menu_open) { ImGui::OpenPopup("appmenu"); app->menu_open = false; }
    if (ImGui::BeginPopup("appmenu")) {
        app->menu_alive = true;
        if (app->popup_close_request) ImGui::CloseCurrentPopup();
        render_commands_items(app);
        ImGui::EndPopup();
    }

    // Dual pane layout: 2-column table guarantees a 50/50 split.
    // Vertical budget comes from the window height and cursor position
    // (GetContentRegionAvail over-reports here); the command bar is then
    // pinned to the window bottom so it can never spill outside.
    const float frame    = ImGui::GetFrameHeightWithSpacing();
    const float bar_h    = frame + 8.0f;  // separator + single-line command bar
    const float tabbar_h = frame;         // tab strip
    const float title_h  = frame;         // pane path bar
    const float sep_h    = 6.0f;          // separator under the path bar
    const float filt_h   = frame + 2.0f;  // quick filter row, when shown
    const float win_h    = ImGui::GetWindowHeight();
    const float table_h  = win_h - ImGui::GetCursorPosY() - bar_h;
    const float base_h   = table_h - tabbar_h - title_h - sep_h;
    const bool  l_filter = left.filter_active || left.filter[0] != '\0' || left.filter_exec_only;
    const bool  r_filter = right.filter_active || right.filter[0] != '\0' || right.filter_exec_only;
    const float left_child_h  = base_h - (l_filter ? filt_h : 0.0f);
    const float right_child_h = base_h - (r_filter ? filt_h : 0.0f);
    ImGui::BeginTable("panes_table", 2,
                      ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInner,
                      ImVec2(0, table_h));
    ImGui::TableNextColumn();

    // Left pane
    render_tab_bar(app, left);
    if (!left.path_edit_active)
        snprintf(left.path_edit, sizeof(left.path_edit), "%s", left.current_dir);
    ImVec4 l_bg   = left_focus ? mc_theme_colors()->title_focused : mc_theme_colors()->title_unfocused;
    ImVec4 l_text = left_focus ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)   : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, l_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, l_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, l_bg);
    ImGui::PushStyleColor(ImGuiCol_Text, l_text);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(1.0f, 1.0f, 1.0f, 0.3f));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (left.path_focus_request) {
        ImGui::SetKeyboardFocusHere();
        left.path_focus_request = false;
    }
    ImGui::InputText("##leftpath", left.path_edit, sizeof(left.path_edit));
    left.path_edit_active = ImGui::IsItemActive();
    if (left.path_edit_active) {
        left_focus = true;
        app->active_path_pane = 0;
    } else if (app->active_path_pane == 0) {
        app->active_path_pane = -1;
    }
    ImGui::PopStyleColor(5);
    ImGui::Separator();

    render_filter_row(app, left, "lfilter");

    float left_w = ImGui::GetContentRegionAvail().x;
    if (app->quick_view_pane && !left_focus)
        render_quick_view_panel(app, left_w, left_child_h, "left_qv");
    else
        render_file_list(app, left, left_focus, left_w, left_child_h, "left_pane");

    ImGui::TableNextColumn();

    // Right pane
    render_tab_bar(app, right);
    if (!right.path_edit_active)
        snprintf(right.path_edit, sizeof(right.path_edit), "%s", right.current_dir);
    ImVec4 r_bg   = !left_focus ? mc_theme_colors()->title_focused : mc_theme_colors()->title_unfocused;
    ImVec4 r_text = !left_focus ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)   : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, r_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, r_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, r_bg);
    ImGui::PushStyleColor(ImGuiCol_Text, r_text);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(1.0f, 1.0f, 1.0f, 0.3f));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (right.path_focus_request) {
        ImGui::SetKeyboardFocusHere();
        right.path_focus_request = false;
    }
    ImGui::InputText("##rightpath", right.path_edit, sizeof(right.path_edit));
    right.path_edit_active = ImGui::IsItemActive();
    if (right.path_edit_active) {
        left_focus = false;
        app->active_path_pane = 1;
    } else if (app->active_path_pane == 1) {
        app->active_path_pane = -1;
    }
    ImGui::PopStyleColor(5);
    ImGui::Separator();

    render_filter_row(app, right, "rfilter");

    float right_w = ImGui::GetContentRegionAvail().x;
    if (app->quick_view_pane && left_focus)
        render_quick_view_panel(app, right_w, right_child_h, "right_qv");
    else
        render_file_list(app, right, !left_focus, right_w, right_child_h, "right_pane");

    ImGui::EndTable();

    // Bottom bar: pinned to the window bottom so it can never overflow.
    // Shows the operation prompt when active, otherwise the command line.
    ImGui::SetCursorPosY(win_h - bar_h);
    ImGui::Separator();
    if (app->op != OP_NONE) {
        const char* label = app->op == OP_RENAME ? "Rename to:"
                          : app->op == OP_NEWFILE ? "New file name:"
                          : "New directory name:";
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetKeyboardFocusHere(-1);
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("##opname", app->op_name, sizeof(app->op_name));
        app->op_input_active = ImGui::IsItemActive();
        if (app->status_msg[0]) {
            ImGui::SameLine();
            ImGui::TextUnformatted(app->status_msg);
        }
    } else {
        // Command line (MC-style: always focused). Status messages show as
        // dimmed text after the input while the command is empty.
        // With no status message the line reports marks and the clipboard,
        // like the TC status line.
        char info[128];
        info[0] = '\0';
        if (app->cmd[0] == '\0' && app->status_msg[0] == '\0') {
            Pane& act = left_focus ? left : right;
            int marks = marked_count(act);
            if (!app->clipboard.paths.empty())
                snprintf(info, sizeof(info), "Marked: %d   Clipboard: %d (%s)",
                         marks, (int)app->clipboard.paths.size(),
                         app->clipboard.cut ? "cut" : "copy");
            else if (marks > 0)
                snprintf(info, sizeof(info), "Marked: %d", marks);
        }
        bool show_status = (app->cmd[0] == '\0' && app->status_msg[0] != '\0');
        const char* shown = show_status ? app->status_msg : info;
        float status_w = shown[0] ? ImGui::CalcTextSize(shown).x + 24.0f : 0.0f;
        ImGui::TextUnformatted("$");
        ImGui::SameLine();
        bool allow_cmd_focus = (app->confirm == CONF_NONE) && (app->active_path_pane < 0) && !app->search_input_active;
        if (allow_cmd_focus)
            ImGui::SetKeyboardFocusHere(-1);
        ImGui::SetNextItemWidth(-status_w - 1.0f);
        ImGui::InputText("##cmdline", app->cmd, sizeof(app->cmd));
        if (shown[0]) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            ImGui::TextUnformatted(shown);
            ImGui::PopStyleColor();
        }
    }

    ImGui::End();

    // Floating windows
    if (left.quick_view)  render_quick_view(app, left);
    if (right.quick_view) render_quick_view(app, right);
    render_viewer(app);
    render_output(app);
    render_hotlist(app);
    render_dirhist(app);
    render_cmdhist(app);
    render_volumes(app);
    render_columns_dialog(app);
    render_tree(app);
    render_search(app);
    render_confirm(app);
    render_about(app);
    render_properties(app);
}

AppPanes* app_panes_init() {
    AppPanes* app = new AppPanes();
    const char* home = getenv("HOME");
    if (!home || !*home) home = "/";

    refresh_pane(&app->left, home);
    refresh_pane(&app->right, home);

    for (int p = 0; p < 2; p++) {
        Pane* pane = (p == 0) ? &app->left : &app->right;
        snprintf(pane->history[0], sizeof(pane->history[0]), "%s", home);
        pane->history_count = 1;
        pane->history_pos = 0;
        snprintf(pane->tabs[0].dir, sizeof(pane->tabs[0].dir), "%s", home);
        pane->tab_count = 1;
        pane->tab_active = 0;
    }

    hotlist_load(app);
    return app;
}

// Store the panes' tabs (and therefore directories) into the config so the
// next session can reopen them.
void app_panes_save_state(AppPanes* app, MCConfig* cfg) {
    cfg->view_mode = app->view_mode;
    cfg->show_hidden = app->show_hidden ? 1 : 0;
    cfg->trash = app->trash_mode ? 1 : 0;
    cfg->sort_mode = fs_get_sort_mode();
    snprintf(cfg->filter_preset, sizeof(cfg->filter_preset), "%s", app->filter_preset);
    snprintf(cfg->columns, sizeof(cfg->columns), "%s", app->columns);
    Pane* src[2] = { &app->left, &app->right };
    for (int p = 0; p < 2; p++) {
        Pane& pane = *src[p];
        char (*tabs)[4096] = (p == 0) ? cfg->left_tabs : cfg->right_tabs;
        int& count  = (p == 0) ? cfg->left_tab_count  : cfg->right_tab_count;
        int& active = (p == 0) ? cfg->left_tab_active : cfg->right_tab_active;
        count = pane.tab_count;
        if (count > MC_MAX_TABS) count = MC_MAX_TABS;
        for (int t = 0; t < count; t++)
            snprintf(tabs[t], 4096, "%s", pane.tabs[t].dir);
        active = pane.tab_active;
    }
}

// Restore tabs from the config. Directories that no longer exist are
// dropped; if nothing survives the app defaults from init stay in place.
void app_panes_apply_state(AppPanes* app, const MCConfig* cfg) {
    struct stat st;
    if (cfg->view_mode >= 0 && cfg->view_mode <= 2) app->view_mode = cfg->view_mode;
    app->show_hidden = cfg->show_hidden != 0;
    app->trash_mode = cfg->trash != 0;
    fs_set_show_hidden(app->show_hidden);
    if (cfg->sort_mode >= FS_SORT_NAME && cfg->sort_mode <= FS_SORT_NONE) fs_set_sort_mode(cfg->sort_mode);
    snprintf(app->filter_preset, sizeof(app->filter_preset), "%s", cfg->filter_preset);
    if (cfg->columns[0]) snprintf(app->columns, sizeof(app->columns), "%s", cfg->columns);
    Pane* dst[2] = { &app->left, &app->right };
    for (int p = 0; p < 2; p++) {
        const char (*tabs)[4096] = (p == 0) ? cfg->left_tabs : cfg->right_tabs;
        int count  = (p == 0) ? cfg->left_tab_count  : cfg->right_tab_count;
        int active = (p == 0) ? cfg->left_tab_active : cfg->right_tab_active;
        Pane& pane = *dst[p];
        if (count <= 0) continue;
        if (count > MAX_TABS) count = MAX_TABS;
        if (active < 0 || active >= count) active = 0;

        char dirs[MAX_TABS][4096];
        int n = 0, new_active = 0;
        for (int t = 0; t < count && n < MAX_TABS; t++) {
            if (!tabs[t][0]) continue;
            if (stat(tabs[t], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (t == active) new_active = n;
            snprintf(dirs[n], 4096, "%s", tabs[t]);
            n++;
        }
        if (n == 0) continue;

        for (int i = 0; i < n; i++)
            snprintf(pane.tabs[i].dir, sizeof(pane.tabs[i].dir), "%s", dirs[i]);
        pane.tab_count = n;
        pane.tab_active = new_active;
        refresh_pane(&pane, pane.tabs[pane.tab_active].dir);
        snprintf(pane.history[0], sizeof(pane.history[0]), "%s", pane.current_dir);
        pane.history_count = 1;
        pane.history_pos = 0;
    }
}

void app_panes_commit_path(AppPanes* app) {
    if (app->active_path_pane == 0) {
        navigate_to(app, &app->left, app->left.path_edit);
        app->left.path_edit_active = false;
    } else if (app->active_path_pane == 1) {
        navigate_to(app, &app->right, app->right.path_edit);
        app->right.path_edit_active = false;
    }
    app->active_path_pane = -1;
}

void app_panes_free(AppPanes* app) {
    if (!app) return;
    free_entries(app->left.entries, app->left.entry_count);
    free_entries(app->right.entries, app->right.entry_count);
    delete app;
}

void app_panes_commit_op(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (app->op == OP_MKDIR) {
        char path[4100];
        snprintf(path, sizeof(path), "%s/%s", active.current_dir, app->op_name);
        if (fs_make_dir(path)) {
            snprintf(app->status_msg, sizeof(app->status_msg), "Created %s", path);
            refresh_pane(&active, active.current_dir);
        } else {
            snprintf(app->status_msg, sizeof(app->status_msg), "mkdir failed: %s", strerror(errno));
        }
    } else if (app->op == OP_NEWFILE) {
        char path[4100];
        snprintf(path, sizeof(path), "%s/%s", active.current_dir, app->op_name);
        if (fs_touch(path)) {
            snprintf(app->status_msg, sizeof(app->status_msg), "Created %s", path);
            refresh_pane(&active, active.current_dir);
            spawn_terminal_editor(app, path);
        } else {
            snprintf(app->status_msg, sizeof(app->status_msg), "Cannot create %s: %s",
                     path, strerror(errno));
        }
    } else if (app->op == OP_RENAME) {
        char dst_dir[4096];
        snprintf(dst_dir, sizeof(dst_dir), "%s", active.current_dir);
        int ok = run_on_targets(app, active, fs_move_entry, dst_dir, app->op_name);
        snprintf(app->status_msg, sizeof(app->status_msg), ok ? "Renamed to %s" : "Rename failed: %s",
                 ok ? app->op_name : strerror(errno));
        refresh_pane(&active, active.current_dir);
    }
    app->op = OP_NONE;
    app->op_input_active = false;
}

void app_panes_cancel_op(AppPanes* app) {
    app->op = OP_NONE;
    app->op_name[0] = '\0';
    app->op_input_active = false;
}

// TC Shift+F5: copy the targets alongside themselves in the same directory.
// The copy lands in a free ".copy" / ".copy.1" / ".copy.2" slot.
static void unique_copy_name(const char* dir, const char* base, char* out, size_t cap) {
    for (int n = 0; n < 1000; n++) {
        if (n == 0) snprintf(out, cap, "%s/%s.copy", dir, base);
        else        snprintf(out, cap, "%s/%s.copy.%d", dir, base, n);
        if (!fs_exists(out)) return;
    }
    snprintf(out, cap, "%s/%s.copy", dir, base);
}

static void do_copy_in_place(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    StringList srcs = target_paths(active);
    if (srcs.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to copy");
        return;
    }
    int ok = 0;
    for (const std::string& s : srcs) {
        char dst[4100];
        unique_copy_name(active.current_dir, path_basename(s), dst, sizeof(dst));
        if (fs_copy_entry(s.c_str(), dst)) ok++;
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "%d of %d copied in place",
             ok, (int)srcs.size());
    refresh_pane(&active, active.current_dir);
}

// TC Shift+Ctrl+F5: a link to each target inside the other pane. The link body
// is absolute, so it stays valid wherever the other pane is showing.
static void do_symlink_other(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;
    StringList srcs = target_paths(active);
    if (srcs.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to link");
        return;
    }
    int ok = 0;
    for (const std::string& s : srcs) {
        char dst[4100];
        snprintf(dst, sizeof(dst), "%s/%s", other.current_dir, path_basename(s));
        if (fs_symlink(s.c_str(), dst)) ok++;
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "%d link(s) in %s", ok, other.current_dir);
    refresh_pane(&other, other.current_dir);
}

// TC Ctrl+L: sum up the marked entries (the cursor entry when nothing is marked).
static void count_marked_sizes(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    StringList t = target_paths(active);
    if (t.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to count");
        return;
    }
    long long bytes = 0; long files = 0, dirs = 0;
    for (const std::string& p : t) {
        long long b = 0; long f = 0, d = 0;
        fs_count_tree(p.c_str(), &b, &f, &d);
        bytes += b; files += f; dirs += d;
    }
    snprintf(app->status_msg, sizeof(app->status_msg),
             "Counted %d: %lld bytes, %ld files, %ld dirs", (int)t.size(), bytes, files, dirs);
}

// TC Alt+Shift+Enter: fill the size column for the directories on screen.
// Bounded per press so a huge tree cannot lock the UI; press again to continue.
static const int DIRSIZE_PER_PRESS = 200;

static void fill_dir_sizes(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    int done = 0, left = 0;
    for (int r = 0; r < (int)active.view.size(); r++) {
        int i = active.view[r];
        FileEntry* e = active.entries[i];
        if (!is_directory(e->name, e->mode) || strcmp(e->name, "..") == 0) continue;
        if (i < (int)active.dir_size.size() && active.dir_size[i] >= 0) continue;
        if (done < DIRSIZE_PER_PRESS) { compute_dir_size(active, i); done++; }
        else left++;
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "Dir sizes: %d filled%s", done,
             left ? ", press again for the next batch" : "");
}

// TC quick search (AltGr + a letter): jump to the next entry whose name starts
// with the word typed so far; further letters extend it. It walks the visible
// rows, so a quick filter narrows the search the way it narrows everything else.
static bool quick_search(AppPanes* app, Pane& pane, char ch) {
    if (pane.quick_active) {
        size_t n = strlen(pane.quick);
        if (n + 1 < sizeof(pane.quick)) { pane.quick[n] = ch; pane.quick[n + 1] = '\0'; }
    } else {
        snprintf(pane.quick, sizeof(pane.quick), "%c", ch);
        pane.quick_active = true;
    }
    int n = (int)pane.view.size();
    if (n == 0) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Search: %s (empty list)", pane.quick);
        return true;
    }
    int pos = row_pos(pane, pane.selected);
    if (pos < 0) pos = -1;
    size_t len = strlen(pane.quick);
    for (int step = 1; step <= n; step++) {
        int i = pane.view[(pos + step) % n];
        if (strncasecmp(entry_rel(pane, i), pane.quick, len) == 0) {
            pane.selected = i;
            pane.scroll_request = true;
            snprintf(app->status_msg, sizeof(app->status_msg), "Search: %s", pane.quick);
            return true;
        }
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "Search: %s (no match)", pane.quick);
    return true;
}

// TC's archive keys: Alt+F5 pack, Alt+Shift+F5 pack and delete, Alt+F6 and
// Alt+F9 extract, Alt+Shift+F9 test. Archives are not implemented (they need a
// third-party backend), but the bindings stay reserved: without this, Alt+F5
// falls through to plain F5 and quietly copies instead.
static bool is_archive_key(SDL_Keycode key, SDL_Keymod mod) {
    if (!(mod & SDL_KMOD_ALT)) return false;
    return key == SDLK_F5 || key == SDLK_F6 || key == SDLK_F9;
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

// Keys the app handles itself (never forwarded to ImGui text input).
// Everything else (printable characters) goes to the command line.
bool app_key_is_special(SDL_Keycode key, SDL_Keymod mod, AppPanes* app) {
    // True while a pane's quick filter field holds the keyboard: typing then
    // belongs to the filter, not to the marks or the command line.
    auto filter_focused = [app]() {
        return app->left.filter_field_active || app->right.filter_field_active;
    };
    switch (key) {
    case SDLK_UP: case SDLK_DOWN:
    case SDLK_PAGEUP: case SDLK_PAGEDOWN:
    case SDLK_HOME: case SDLK_END:
    case SDLK_TAB: case SDLK_RETURN: case SDLK_KP_ENTER:
    case SDLK_ESCAPE: case SDLK_INSERT: case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4:
    case SDLK_F5: case SDLK_F6: case SDLK_F7: case SDLK_F8:
    case SDLK_F9: case SDLK_F10: case SDLK_F11: case SDLK_F12:
    case SDLK_MENU:
        return true;
    case SDLK_BACKSPACE:
        return app->cmd[0] == '\0' && !filter_focused();   // empty cmd: go to parent dir
    case SDLK_DELETE:
        return !filter_focused();   // Del edits the filter text, it does not delete files
    case SDLK_SPACE:
        return app->cmd[0] == '\0' && !filter_focused();   // TC: Space marks on an empty cmd
    case SDLK_SLASH: case SDLK_KP_DIVIDE:
        return app->cmd[0] == '\0' && !filter_focused();   // TC Num/: restore the previous marks
    case SDLK_PLUS: case SDLK_KP_PLUS:
    case SDLK_MINUS: case SDLK_KP_MINUS:
    case SDLK_ASTERISK: case SDLK_KP_MULTIPLY:
        return app->cmd[0] == '\0' && !filter_focused();   // select shortcuts need an empty cmd
    case SDLK_C: case SDLK_X: case SDLK_V:
        // TC file clipboard keys. With text already in the command line they
        // are text editing keys, so they fall through to ImGui.
        return (mod & SDL_KMOD_CTRL) && !(mod & SDL_KMOD_ALT) && app->cmd[0] == '\0';
    default:
        break;
    }
    if (mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_MODE)) return true;
    return false;
}

void app_panes_handle_key(AppPanes* app, SDL_Keycode key, SDL_Keymod mod) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;
    bool shift = (mod & SDL_KMOD_SHIFT) != 0;

    // ---- modal confirm dialog ----
    if (app->confirm != CONF_NONE) {
        if (key == SDLK_Y || key == SDLK_RETURN || key == SDLK_KP_ENTER) confirm_yes(app);
        else if (key == SDLK_N || key == SDLK_ESCAPE) {
            app->confirm = CONF_NONE;
            app->confirm_text[0] = '\0';
        }
        return;
    }

    // ---- full viewer keys ----
    if (app->viewer_open) {
        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER ||
            key == SDLK_Q) { app->viewer_open = false; return; }
        if (key == SDLK_PAGEDOWN) { viewer_scroll += 300.0f; viewer_dirty = true; return; }
        if (key == SDLK_PAGEUP)   { viewer_scroll -= 300.0f; viewer_dirty = true; return; }
        if (key == SDLK_DOWN)     { viewer_scroll += 20.0f;  viewer_dirty = true; return; }
        if (key == SDLK_UP)       { viewer_scroll -= 20.0f;  viewer_dirty = true; return; }
        if (key == SDLK_HOME)     { viewer_scroll = 0;       viewer_dirty = true; return; }
        if (key == SDLK_END)      { viewer_scroll = 1e9f;    viewer_dirty = true; return; }
        return;
    }

    // TC Shift+Esc: minimize the window.
    if (key == SDLK_ESCAPE && shift && app->sdl_window) {
        SDL_MinimizeWindow((SDL_Window*)app->sdl_window);
        return;
    }

    // ---- other windows: Esc closes ----
    if (app->output_open && (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER)) {
        app->output_open = false; return;
    }
    if (app->search_open && key == SDLK_ESCAPE) {
        app->search_open = false; app->search_input_active = false; return;
    }
    if (app->hotlist_open && key == SDLK_ESCAPE) { app->hotlist_open = false; return; }
    if (app->dirhist_open && key == SDLK_ESCAPE) { app->dirhist_open = false; return; }
    if (app->cmdhist_open && key == SDLK_ESCAPE) { app->cmdhist_open = false; return; }
    if (app->tree_open && key == SDLK_ESCAPE) { app->tree_open = false; return; }
    if (app->about_open && key == SDLK_ESCAPE)   { app->about_open = false; return; }
    if (app->props_open && (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER)) {
        app->props_open = false; return;
    }

    if (key == SDLK_ESCAPE && active.quick_active) {
        active.quick_active = false;
        active.quick[0] = '\0';
        return;
    }

    // ---- TC quick search: AltGr + a letter. AltGr is SDL_KMOD_MODE, or the
    //      right Alt on layouts without a real AltGr. Checked before the Alt
    //      bindings so AltGr+Q searches instead of quitting. ----
    if ((mod & (SDL_KMOD_MODE | SDL_KMOD_RALT)) && key >= SDLK_A && key <= SDLK_Z) {
        quick_search(app, active, (char)tolower((int)key));
        return;
    }

    // ---- quit (TC Alt+F4; Alt+Q kept as an alias) ----
    if ((key == SDLK_F4 || key == SDLK_Q) && (mod & SDL_KMOD_ALT)) { app->quit_request = true; return; }

    // ---- TC Shift+F1: define the columns of the long view ----
    if (key == SDLK_F1 && shift && !(mod & (SDL_KMOD_ALT | SDL_KMOD_CTRL))) {
        app->columns_open = !app->columns_open;
        return;
    }

    // ---- help (TC F1) ----
    if (key == SDLK_F1 && !(mod & (SDL_KMOD_ALT | SDL_KMOD_CTRL))) {
        app->about_open = !app->about_open;
        return;
    }

    // ---- TC quick filter: Ctrl+S opens it, Ctrl+Shift+S re-applies the last
    //      filter, Ctrl+F10 clears it (checked before the F10 menu) ----
    if (key == SDLK_S && (mod & SDL_KMOD_CTRL)) {
        if (shift && active.last_filter[0]) {
            snprintf(active.filter, sizeof(active.filter), "%s", active.last_filter);
            active.filter_active = true;
            snprintf(app->status_msg, sizeof(app->status_msg), "Filter: %s", active.filter);
        } else {
            active.filter_active = true;
            active.filter_focus_request = true;
        }
        return;
    }
    if (key == SDLK_F10 && (mod & SDL_KMOD_CTRL)) {
        active.filter[0] = '\0';
        active.filter_exec_only = false;
        active.filter_active = false;
        active.filter_field_active = false;
        ImGui::ClearActiveID();
        return;
    }
    // Ctrl+F11: executables only. Ctrl+F12: the preset from the config file.
    if (key == SDLK_F11 && (mod & SDL_KMOD_CTRL)) {
        active.filter_exec_only = !active.filter_exec_only;
        active.filter_active = active.filter_exec_only || active.filter[0] != '\0';
        snprintf(app->status_msg, sizeof(app->status_msg), "Executables only: %s",
                 active.filter_exec_only ? "on" : "off");
        return;
    }
    if (key == SDLK_F12 && (mod & SDL_KMOD_CTRL)) {
        if (!app->filter_preset[0]) {
            snprintf(app->status_msg, sizeof(app->status_msg),
                     "No filter preset (set filter_preset in the config)");
        } else {
            snprintf(active.filter, sizeof(active.filter), "%s", app->filter_preset);
            active.filter_active = true;
            snprintf(app->status_msg, sizeof(app->status_msg), "Filter: %s", active.filter);
        }
        return;
    }

    // ---- menus (TC F10 = app menu, F9 = menu above the pane) ----
    // Alt+F10: the directory tree of the focused pane.
    if (key == SDLK_F10 && (mod & SDL_KMOD_ALT)) {
        if (app->tree_open) app->tree_open = false;
        else tree_open(app, active);
        return;
    }
    // Shift+F10 and the Menu key: the local menu at the cursor row.
    if ((key == SDLK_F10 && shift) || key == SDLK_MENU) {
        app->menu_open = false;
        app->pane_menu_open = false;
        if (app->ctx_menu_alive) app->popup_close_request = true;
        else app->entry_menu_open = true;
        return;
    }
    if (is_archive_key(key, mod)) {
        const char* what = (key == SDLK_F5) ? (shift ? "Pack and delete" : "Pack")
                       : (key == SDLK_F9 && shift) ? "Test archive"
                       : "Extract from archive";
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "%s: archives are not supported yet", what);
        return;
    }
    if (key == SDLK_F10) {
        if (app->menu_alive) app->popup_close_request = true; else app->menu_open = true;
        app->pane_menu_open = false;
        return;
    }
    if (key == SDLK_F9 && !shift && !app->pane_menu_alive && !(mod & (SDL_KMOD_ALT | SDL_KMOD_CTRL))) {
        app->pane_menu_open = true;
        app->menu_open = false;
        return;
    }

    // ---- TC Alt+F11/F12: keyboard-focus the left/right path bar ----
    if (key == SDLK_F11 && (mod & SDL_KMOD_ALT)) {
        app->left.path_focus_request = true;
        app->left_focus = true;
        return;
    }
    if (key == SDLK_F12 && (mod & SDL_KMOD_ALT)) {
        app->right.path_focus_request = true;
        app->left_focus = false;
        return;
    }

    // ---- TC Alt+F8: the command line history list ----
    if (key == SDLK_F8 && (mod & SDL_KMOD_ALT)) { app->cmdhist_open = !app->cmdhist_open; return; }

    // ---- TC Alt+F1/F2: choose the left/right drive ----
    if (key == SDLK_F1 && (mod & SDL_KMOD_ALT)) {
        app->volumes_open = !app->volumes_open;
        app->volumes_target = 0;
        return;
    }
    if (key == SDLK_F2 && (mod & SDL_KMOD_ALT)) {
        app->volumes_open = !app->volumes_open;
        app->volumes_target = 1;
        return;
    }

    // ---- tabs (Ctrl+T new, Ctrl+W close, Ctrl+Tab cycle, Ctrl+1..9 jump) ----
    if (key == SDLK_T && (mod & SDL_KMOD_CTRL)) { tab_new(&active, !shift); return; }
    if (key == SDLK_W && (mod & SDL_KMOD_CTRL)) {
        if (shift) tab_close_all(&active); else tab_close(&active);
        return;
    }
    if ((mod & SDL_KMOD_CTRL) && key >= SDLK_1 && key <= SDLK_9) {
        tab_switch(&active, (int)(key - SDLK_1)); return;
    }
    if ((mod & SDL_KMOD_CTRL) && key == SDLK_TAB) {
        tab_cycle(&active, shift ? -1 : 1);
        return;
    }

    // ---- TC Ctrl+PgUp = parent directory, Ctrl+PgDn = open entry under cursor ----
    if ((mod & SDL_KMOD_CTRL) && key == SDLK_PAGEUP) {
        char parentbuf[4096];
        snprintf(parentbuf, sizeof(parentbuf), "%s", active.current_dir);
        navigate_to(app, &active, dirname(parentbuf));
        return;
    }
    if ((mod & SDL_KMOD_CTRL) && key == SDLK_PAGEDOWN) {
        if (active.selected >= 0 && active.selected < active.entry_count)
            activate_entry(app, active, active.selected);
        return;
    }

    // ---- TC Ctrl+Up / Ctrl+Shift+Down: cursor entry in a new tab ----
    if (key == SDLK_UP && (mod & SDL_KMOD_CTRL)) { open_cursor_in_new_tab(app, &active, true); return; }
    if (key == SDLK_DOWN && (mod & SDL_KMOD_CTRL) && shift) {
        open_cursor_in_new_tab(app, &other, true);
        return;
    }

    // ---- TC Ctrl+Left/Right: show the cursor directory in the other pane ----
    if ((mod & SDL_KMOD_CTRL) && (key == SDLK_LEFT || key == SDLK_RIGHT)) {
        open_cursor_in_other_pane(app, &active, &other);
        return;
    }

    // ---- Ctrl+, to the filesystem root (Shift: the other pane) ----
    if (key == SDLK_COMMA && (mod & SDL_KMOD_CTRL)) {
        navigate_to(app, shift ? &other : &active, "/");
        return;
    }

    // ---- Alt+Down: the directories this pane has visited ----
    if (key == SDLK_DOWN && (mod & SDL_KMOD_ALT)) { app->dirhist_open = !app->dirhist_open; return; }

    // ---- directory history ----
    if (key == SDLK_LEFT  && (mod & SDL_KMOD_ALT)) { history_back(app, &active); return; }
    if (key == SDLK_RIGHT && (mod & SDL_KMOD_ALT)) { history_forward(app, &active); return; }

    // ---- command history (Alt+PgUp / Alt+PgDn) ----
    if (key == SDLK_PAGEUP && (mod & SDL_KMOD_ALT)) {
        if (app->cmd_hist_count > 0) {
            if (app->cmd_hist_pos > 0) app->cmd_hist_pos--;
            snprintf(app->cmd, sizeof(app->cmd), "%s", app->cmd_history[app->cmd_hist_pos]);
        }
        return;
    }
    if (key == SDLK_PAGEDOWN && (mod & SDL_KMOD_ALT)) {
        if (app->cmd_hist_pos < app->cmd_hist_count - 1) {
            app->cmd_hist_pos++;
            snprintf(app->cmd, sizeof(app->cmd), "%s", app->cmd_history[app->cmd_hist_pos]);
        } else {
            app->cmd_hist_pos = app->cmd_hist_count;
            app->cmd[0] = '\0';
        }
        return;
    }

    // ---- hotlist ----
    if (key == SDLK_F9 && shift) { app->hotlist_open = !app->hotlist_open; return; }
    if (key == SDLK_D && (mod & SDL_KMOD_ALT)) {
        bool dup = false;
        for (const std::string& s : app->hotlist) if (s == active.current_dir) { dup = true; break; }
        if (!dup) {
            app->hotlist.push_back(active.current_dir);
            hotlist_save(app);
            snprintf(app->status_msg, sizeof(app->status_msg), "Added to hotlist: %s", active.current_dir);
        }
        return;
    }

    // ---- search / quick cd / hidden / view / sort ----
    // TC Alt+F7 = search files (Shift+F7 kept as an alias)
    if (key == SDLK_F7 && (shift || (mod & SDL_KMOD_ALT))) { app->search_open = !app->search_open; return; }
    if (key == SDLK_C && (mod & SDL_KMOD_ALT)) { snprintf(app->cmd, sizeof(app->cmd), "cd "); return; }
    if (key == SDLK_PERIOD && (mod & SDL_KMOD_ALT)) {
        app->show_hidden = !app->show_hidden;
        fs_set_show_hidden(app->show_hidden);
        app_panes_refresh_left(app);
        app_panes_refresh_right(app);
        return;
    }
    if (key == SDLK_O && (mod & SDL_KMOD_CTRL)) {
        app->view_mode = (app->view_mode + 1) % 3;
        static const char* names[] = {"mid", "brief", "full"};
        snprintf(app->status_msg, sizeof(app->status_msg), "View mode: %s", names[app->view_mode]);
        return;
    }
    // ---- TC sort keys: Ctrl+F3 name, Ctrl+F4 ext, Ctrl+F5 date, Ctrl+F6 size,
    //      Ctrl+F7 unsorted (readdir order) ----
    if ((mod & SDL_KMOD_CTRL) && key >= SDLK_F3 && key <= SDLK_F7) {
        static const int map[5] = { FS_SORT_NAME, FS_SORT_EXT, FS_SORT_DATE,
                                    FS_SORT_SIZE, FS_SORT_NONE };
        static const char* names[5] = { "name", "ext", "date", "size", "unsorted" };
        fs_set_sort_mode(map[key - SDLK_F3]);
        snprintf(app->status_msg, sizeof(app->status_msg), "Sort by: %s", names[key - SDLK_F3]);
        app_panes_refresh_left(app);
        app_panes_refresh_right(app);
        return;
    }
    // TC Shift+F3: view only the entry under the cursor even when several are
    // marked (the marks are left untouched).
    if (key == SDLK_F3 && shift) {
        if (active.selected >= 0 && active.selected < active.entry_count) {
            FileEntry* e = active.entries[active.selected];
            if (is_directory(e->name, e->mode)) {
                snprintf(app->status_msg, sizeof(app->status_msg), "Shift+F3: not a file");
            } else {
                char full[4100];
                snprintf(full, sizeof(full), "%s/%s", active.current_dir, entry_rel(active, active.selected));
                open_viewer(app, full);
                if (marked_count(active) > 1)
                    snprintf(app->status_msg, sizeof(app->status_msg),
                             "Viewing only %s (%d marked ignored)", e->name, marked_count(active));
            }
        }
        return;
    }
    if (key == SDLK_R && (mod & SDL_KMOD_CTRL)) {
        app->left_focus ? app_panes_refresh_left(app) : app_panes_refresh_right(app);
        return;
    }

    // ---- panes (TC Ctrl+U swaps the pane directories; Ctrl+Shift+U also
    //      swaps the tab strips). Clearing marks moved to Ctrl+Num-. ----
    if (key == SDLK_U && (mod & SDL_KMOD_CTRL)) { swap_panes(app, shift); return; }
    // TC Ctrl+B: branch view of the whole subtree; Ctrl+Shift+B branches only
    // the marked directories. Pressing either again leaves branch view.
    if (key == SDLK_B && (mod & SDL_KMOD_CTRL)) { branch_toggle(app, active, shift); return; }
    // TC Ctrl+L: sum up the marked entries.
    if (key == SDLK_L && (mod & SDL_KMOD_CTRL)) { count_marked_sizes(app); return; }

    // ---- marking ----
    if (key == SDLK_SPACE && (mod & SDL_KMOD_CTRL)) { toggle_mark(active, active.selected); return; }
    // TC Space: mark/unmark. On a not-yet-marked directory it also sums the
    // directory so its size replaces "-" in the size column.
    if (key == SDLK_SPACE) {
        if (active.selected >= 0 && active.selected < active.entry_count) {
            bool was_marked = active.selected < (int)active.marked.size() && active.marked[active.selected];
            FileEntry* e = active.entries[active.selected];
            toggle_mark(active, active.selected);
            if (!was_marked && is_directory(e->name, e->mode)) compute_dir_size(active, active.selected);
        }
        return;
    }
    if (key == SDLK_INSERT) {
        toggle_mark(active, active.selected);
        int nxt = nav_step(active, active.selected, 1);
        if (nxt >= 0) { active.selected = nxt; active.scroll_request = true; }
        return;
    }
    if (key == SDLK_A && (mod & SDL_KMOD_CTRL)) { select_all_marks(active); return; }
    if (key == SDLK_I && (mod & SDL_KMOD_CTRL)) { invert_marks(active); return; }
    if (mod & SDL_KMOD_CTRL) {
        // Ctrl+Num+ all, Ctrl+Num- none; the Shift+minus forms touch files only.
        if (key == SDLK_MINUS || key == SDLK_KP_MINUS) { clear_marks(active, shift ? SCOPE_FILES : SCOPE_ALL); return; }
        if (key == SDLK_PLUS || key == SDLK_KP_PLUS) { select_all_marks(active); return; }
        if (key == SDLK_ASTERISK || key == SDLK_KP_MULTIPLY) { invert_marks(active); return; }
    } else if (app->cmd[0] == '\0') {
        // Num+ / Num- extend and shrink the selection row by row.
        if (key == SDLK_PLUS || key == SDLK_KP_PLUS) { step_marks(active, true); return; }
        if (key == SDLK_MINUS || key == SDLK_KP_MINUS) { step_marks(active, false, shift ? SCOPE_FILES : SCOPE_ALL); return; }
        if (key == SDLK_ASTERISK || key == SDLK_KP_MULTIPLY) { invert_marks(active); return; }
        // Num/: walk the mark set back to the state before the last bulk op.
        if (key == SDLK_SLASH || key == SDLK_KP_DIVIDE) { restore_marks(active); return; }
    }
    // Alt+Num+ / Alt+Num-: mark / unmark entries sharing the cursor's extension
    if (mod & SDL_KMOD_ALT) {
        if (key == SDLK_PLUS || key == SDLK_KP_PLUS) { mark_same_ext(active, true); return; }
        if (key == SDLK_MINUS || key == SDLK_KP_MINUS) { mark_same_ext(active, false); return; }
    }

    // ---- TC internal file clipboard (Ctrl+C / Ctrl+X / Ctrl+V) ----
    if ((mod & SDL_KMOD_CTRL) && !(mod & SDL_KMOD_ALT)) {
        if (key == SDLK_C) { clipboard_set(app, active, false); return; }
        if (key == SDLK_X) { clipboard_set(app, active, true); return; }
        if (key == SDLK_V) { do_paste(app); return; }
    }

    // ---- file operations ----
    // Alt+F3: open the cursor entry with the desktop's default application.
    if (key == SDLK_F3 && (mod & SDL_KMOD_ALT)) {
        if (active.selected >= 0 && active.selected < active.entry_count) {
            char full[4100];
            snprintf(full, sizeof(full), "%s/%s", active.current_dir,
                     active.entries[active.selected]->name);
            open_with_default(app, full);
        }
        return;
    }
    // TC "file view short / long": the brief and the full column set.
    if (key == SDLK_F1 && (mod & SDL_KMOD_CTRL)) {
        app->view_mode = VIEW_BRIEF;
        snprintf(app->status_msg, sizeof(app->status_msg), "View: brief");
        return;
    }
    if (key == SDLK_F2 && (mod & SDL_KMOD_CTRL)) {
        app->view_mode = VIEW_FULL;
        snprintf(app->status_msg, sizeof(app->status_msg), "View: full");
        return;
    }
    if (key == SDLK_F3) { active.quick_view = !active.quick_view; return; }
    if (key == SDLK_DELETE) { request_delete(app, shift); return; }

    // TC Shift+F2: mark the differences between the two panes.
    if (key == SDLK_F2 && shift) {
        app->compare_on = !app->compare_on;
        snprintf(app->status_msg, sizeof(app->status_msg), "Compare panes: %s",
                 app->compare_on ? "on" : "off");
        return;
    }
    // TC Ctrl+Q: the inactive pane becomes a quick view panel.
    if (key == SDLK_Q && (mod & SDL_KMOD_CTRL)) {
        app->quick_view_pane = !app->quick_view_pane;
        snprintf(app->status_msg, sizeof(app->status_msg), "Quick view panel: %s",
                 app->quick_view_pane ? "on" : "off");
        return;
    }

    if (key == SDLK_F2 && !(mod & (SDL_KMOD_ALT | SDL_KMOD_CTRL))) {
        if (branch_blocked(app, active)) return;
        if (active.selected < active.entry_count && strcmp(active.entries[active.selected]->name, "..") != 0) {
            app->op = OP_RENAME;
            snprintf(app->op_name, sizeof(app->op_name), "%s", active.entries[active.selected]->name);
        }
        return;
    }
    if (key == SDLK_F7) {
        if (branch_blocked(app, active)) return;
        app->op = OP_MKDIR;
        app->op_name[0] = '\0';
        return;
    }
    // TC Shift+F4: create an empty file and open it in the editor.
    if (key == SDLK_F4 && shift) {
        if (branch_blocked(app, active)) return;
        app->op = OP_NEWFILE;
        app->op_name[0] = '\0';
        return;
    }
    if (key == SDLK_F4) {
        if (active.selected < active.entry_count) {
            FileEntry* e = active.entries[active.selected];
            if (!is_directory(e->name, e->mode)) {
                char full[4100];
                snprintf(full, sizeof(full), "%s/%s", active.current_dir, entry_rel(active, active.selected));
                spawn_terminal_editor(app, full);
            }
        }
        return;
    }
    // TC "F8, Del": plain Del deletes; Shift+Del / Shift+F8 skip the trash
    // (wired up in the trash phase, both confirm first today).
    // TC Ctrl+Shift+F8: the tree takes over this panel. Shift+Tab splits it.
    if (key == SDLK_F8 && (mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) == (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) {
        tree_mode_toggle(app, active, TREE_EMBED);
        return;
    }
    if (key == SDLK_F8 && !(mod & (SDL_KMOD_ALT | SDL_KMOD_CTRL))) { request_delete(app, shift); return; }
    // Ctrl+F8: the directory tree, like Alt+F10.
    if (key == SDLK_F8 && (mod & SDL_KMOD_CTRL)) {
        if (app->tree_open) app->tree_open = false;
        else tree_open(app, active);
        return;
    }
    // TC Shift+Ctrl+F5: link the targets into the other pane.
    if (key == SDLK_F5 && shift && (mod & SDL_KMOD_CTRL)) { do_symlink_other(app); return; }
    // TC Shift+F5: copy the targets alongside themselves.
    if (key == SDLK_F5 && shift) { do_copy_in_place(app); return; }
    // TC Shift+F6: move/rename inside the current directory = the rename prompt.
    if (key == SDLK_F6 && shift) {
        if (branch_blocked(app, active)) return;
        if (active.selected < active.entry_count && strcmp(active.entries[active.selected]->name, "..") != 0) {
            app->op = OP_RENAME;
            snprintf(app->op_name, sizeof(app->op_name), "%s", active.entries[active.selected]->name);
        }
        return;
    }
    if (key == SDLK_F5 && !(mod & (SDL_KMOD_ALT | SDL_KMOD_CTRL))) { do_copy(app); return; }
    if (key == SDLK_F6 && !(mod & (SDL_KMOD_ALT | SDL_KMOD_CTRL))) { do_move(app); return; }

    // ---- navigation (all moves walk the visible rows, i.e. the filter) ----
    if (key == SDLK_UP) {
        if (shift) toggle_mark(active, active.selected);
        int n = nav_step(active, active.selected, -1);
        if (n >= 0) { active.selected = n; active.scroll_request = true; }
        active.anchor = active.selected;
        return;
    }
    if (key == SDLK_DOWN) {
        if (shift) toggle_mark(active, active.selected);
        int n = nav_step(active, active.selected, 1);
        if (n >= 0) { active.selected = n; active.scroll_request = true; }
        active.anchor = active.selected;
        return;
    }
    if (key == SDLK_PAGEUP) {
        int old = active.selected;
        int n = nav_page(active, old, -1, active.page_size);
        if (n < 0) n = first_visible(active);
        if (n >= 0) {
            if (shift) toggle_range(active, old, n);
            active.selected = n;
            active.scroll_request = true;
            active.anchor = n;
        }
        return;
    }
    if (key == SDLK_PAGEDOWN) {
        int old = active.selected;
        int n = nav_page(active, old, 1, active.page_size);
        if (n < 0) n = last_visible(active);
        if (n >= 0) {
            if (shift) toggle_range(active, old, n);
            active.selected = n;
            active.scroll_request = true;
            active.anchor = n;
        }
        return;
    }
    if (key == SDLK_HOME) {
        if (shift) {
            const char* home = getenv("HOME");
            if (home && *home) navigate_to(app, &active, home);
        } else {
            int n = first_visible(active);
            if (n >= 0) { active.selected = n; active.scroll_request = true; }
        }
        return;
    }
    if (key == SDLK_END) {
        int n = last_visible(active);
        if (n >= 0) { active.selected = n; active.scroll_request = true; }
        return;
    }
    if (key == SDLK_TAB && shift) { tree_mode_toggle(app, active, TREE_SPLIT); return; }
    if (key == SDLK_TAB) { app->left_focus = !app->left_focus; return; }
    if (key == SDLK_BACKSPACE) {
        char parentbuf[4096];
        snprintf(parentbuf, sizeof(parentbuf), "%s", active.current_dir);
        char* parent = dirname(parentbuf);
        navigate_to(app, &active, parent);
        return;
    }
    // TC Alt+Shift+Enter: compute the size of every directory on screen.
    // ---- TC tree panel: while a tree replaces the list, the keyboard drives
    //      the tree instead of the file rows. ----
    if (active.tree_mode == TREE_EMBED && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
        if (tree_nav(app, active, key)) return;
    }

    if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && (mod & SDL_KMOD_ALT) && shift) {
        fill_dir_sizes(app);
        return;
    }
    if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && (mod & SDL_KMOD_ALT)) {
        open_properties(app, active);
        return;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        active.quick_active = false;   // the search word is spent
        // Enter in the quick filter commits it and leaves the field; the filter
        // itself stays applied.
        if (active.filter_field_active) {
            snprintf(active.last_filter, sizeof(active.last_filter), "%s", active.filter);
            active.filter_field_active = false;
            ImGui::ClearActiveID();
            return;
        }
        // Non-empty command line: Enter runs the command, not the file entry.
        if (app->cmd[0] != '\0') { exec_command(app); return; }
        if (active.selected >= 0 && active.selected < active.entry_count)
            activate_entry(app, active, active.selected);
        return;
    }
    if (key == SDLK_ESCAPE) {
        // A keyboard-opened menu closes first (TC F10 toggles, Esc dismisses).
        if (app->menu_alive || app->pane_menu_alive) {
            app->popup_close_request = true;
            app->menu_open = app->pane_menu_open = false;
            return;
        }
        // A quick filter is cleared before the command line (TC Esc).
        if (active.filter_active || active.filter[0] || active.filter_exec_only) {
            active.filter[0] = '\0';
            active.filter_exec_only = false;
            active.filter_active = false;
            active.filter_field_active = false;
            ImGui::ClearActiveID();
            return;
        }
        app->cmd[0] = '\0';
        app->status_msg[0] = '\0';
        app->cmd_hist_pos = app->cmd_hist_count;
        return;
    }
}
