#include "panes.h"
#include "filesystem.h"
#include "archive.h"
#include "theme.h"
#include "config.h"
#include "version.h"   // generated: MC_APP_NAME, MC_VERSION
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>   // ClearActiveID: drop the filter field's keyboard focus
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#ifdef MC_HAVE_FREETYPE
#include <imgui_freetype.h>       // ImGuiFreeType::GetFontLoader() and the loader flags
#include <ft2build.h>             // FreeType's include shim, then the header with its version
#include FT_FREETYPE_H
#endif
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
    left.archive_path[0] = '\0'; left.archive_prefix[0] = '\0';
    right.archive_path[0] = '\0'; right.archive_prefix[0] = '\0';
    left.remote_root[0] = '\0'; left.remote_path[0] = '\0'; left.remote_came_from[0] = '\0';
    right.remote_root[0] = '\0'; right.remote_path[0] = '\0'; right.remote_came_from[0] = '\0';
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
    op_error[0] = '\0';
    op_focus_request = false;
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
    remote_mgr_open = false;
    remote_quick_open = false;
    remote_quick[0] = '\0';
    remote_note[0] = '\0';
    remote_edit = -1;
    remote_cursor = -1;
    remote_focus_request = false;
    search_open = false;
    search_pattern[0] = '\0';
    search_done = false;
    search_input_active = false;
    viewer_open = false;
    viewer_path[0] = '\0';
    viewer_tmp_dir[0] = '\0';
    clipboard.archive[0] = '\0';
    pack_open = false;
    pack_name[0] = '\0';
    pack_dir[0] = '\0';
    pack_kind_idx = 0;
    pack_level = 6;
    pack_use_level = false;
    pack_password[0] = '\0';
    pack_volume_bytes = 0;
    pack_delete = false;
    props_open = false;
    props_path[0] = '\0';
    props_size = 0;
    props_files = 0;
    props_dirs = 0;
    about_open = false;
    options_open = false;
    options_field_active = false;
    focus_claimed = false;
    font_dirty = false;
    font_scanned = false;
    font_size = 0.0f;
    font[0] = '\0';
    snprintf(hinting, sizeof(hinting), "default");
    display_scale = 1.0f;
    font_filter[0] = '\0';
    font_msg[0] = '\0';
    snprintf(theme, sizeof(theme), "dark");
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
    prog_abort = false;
    prog_watch = nullptr;
    prog_handoff = false;
    op_run_bg = false;
    pending_op = DEFER_NONE;
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
// Path of a row inside its pane: the full relative path in branch view and in
// an archive, the plain name otherwise. Every path and every label built from a
// listing goes through this.
static const char* entry_rel(const Pane& p, int idx) {
    if ((p.list_mode == LIST_BRANCH || p.list_mode == LIST_ARCHIVE) &&
        idx >= 0 && idx < (int)p.rel.size())
        return p.rel[idx].c_str();
    return p.entries[idx]->name;
}

// The pane is showing archive members instead of a directory. Everything below
// that path is virtual, so these guards protect the operations that would
// otherwise build a real path out of it.
static bool in_archive(const Pane& pane) { return pane.archive_path[0] != '\0'; }

static bool archive_readonly(AppPanes* app, const Pane& pane) {
    if (!in_archive(pane)) return false;
    snprintf(app->status_msg, sizeof(app->status_msg),
             "Archive contents are read-only: Alt+F9 extracts, Alt+F5 packs");
    return true;
}

// Branch view is a flattened snapshot of a subtree, so anything that resolves a
// row against the pane's directory has to stay out of it. entry_rel() is what
// makes paths correct in branch view; rename, mkdir and the rest are not.
static bool branch_blocked(AppPanes* app, const Pane& pane) {
    if (pane.list_mode == LIST_ARCHIVE) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Not available inside an archive (Backspace leaves it)");
        return true;
    }
    if (pane.list_mode != LIST_BRANCH) return false;
    snprintf(app->status_msg, sizeof(app->status_msg), "Not available in branch view (Ctrl+B exits)");
    return true;
}

// The pane is showing a directory that lives on a site. Like an archive, every
// path built from a row is virtual, so the operations below have to go through
// the transport rather than hand the path to a filesystem call.
static bool in_remote(const Pane& pane) { return pane.remote_root[0] != '\0'; }

// A pane's virtual path carries no password, so the login has to be looked up
// from the site list. refresh_pane() is called from far too many places to gain
// an AppPanes parameter, so the list is bound here instead: two pointers into
// the AppPanes that owns it, which stay valid because the vectors are edited in
// place. app_panes_free() unbinds them so a stale pane can never read a dead
// app's sites.
static const std::vector<NetSite>* g_net_saved;
static const std::vector<NetSite>* g_net_adhoc;
// The last listing failure. refresh_pane() has no status line of its own, so it
// leaves the reason here and the render loop moves it into the status bar.
static std::string g_net_note;

static void net_bind(const AppPanes* app) {
    g_net_saved = app ? &app->remote_sites : nullptr;
    g_net_adhoc = app ? &app->remote_adhoc : nullptr;
}

// The site a root URL belongs to. The root carries the user, so two entries for
// one host with different logins stay distinct. A quick connect that still holds
// a typed password wins over a saved entry that declined to remember one.
static const NetSite* net_site_for(const std::string& root) {
    const NetSite* loose = nullptr;
    const std::vector<NetSite>* lists[] = { g_net_saved, g_net_adhoc };
    for (const std::vector<NetSite>* list : lists) {
        if (!list) continue;
        for (const NetSite& s : *list) {
            if (net_site_root(s) != root) continue;
            if (!s.password.empty()) return &s;
            loose = &s;
        }
    }
    return loose;
}

// Build the rows for one directory on a site. The listing arrives with names,
// sizes, times and permission bits, so the pane sorts, filters and formats a
// remote row exactly like a local one. `rel` is left alone: a remote row's path
// is its name against the pane's virtual directory, as in an ordinary directory.
static void list_remote_dir(const NetSite& site, const std::string& rpath,
                            FileEntry*** out_entries, int* out_count,
                            std::string* err) {
    *out_entries = nullptr;
    *out_count = 0;

    std::vector<FileEntry> rows;
    if (!net_list(site, rpath, &rows, err)) return;

    std::vector<FileEntry*> ptrs;
    for (const FileEntry& e : rows) {
        if (!fs_get_show_hidden() && e.name[0] == '.') continue;
        FileEntry* fe = file_entry_new(e.name, e.mode, e.size, e.mtime);
        if (fe) ptrs.push_back(fe);
    }
    // Same order a real directory uses, so the cursor and the sort keys behave
    // identically on a site.
    std::sort(ptrs.begin(), ptrs.end(), [](const FileEntry* a, const FileEntry* b) {
        return compare_files(&a, &b) < 0;
    });

    // ".." is always a row. At the site root it has no parent on the server, so
    // it leaves the site; activate_entry() is what knows that.
    *out_entries = (FileEntry**)calloc(ptrs.size() + 1, sizeof(FileEntry*));
    if (!*out_entries) {
        for (FileEntry* fe : ptrs) file_entry_free(fe);
        return;
    }
    (*out_entries)[0] = file_entry_new("..", S_IFDIR | 0755, 0, 0);
    int i = 1;
    for (FileEntry* fe : ptrs) (*out_entries)[i++] = fe;
    *out_count = i;
}

// The sites file is read once, before any pane is refreshed: a session restored
// from the config can hold remote paths in its tabs, and those need their
// logins back before the first listing.
static void remote_sites_load(AppPanes* app) {
    std::string err;
    if (!net_sites_load(&app->remote_sites, &err) && !err.empty())
        snprintf(app->remote_note, sizeof(app->remote_note), "%s", err.c_str());
}

static void remote_sites_write(AppPanes* app) {
    std::string err;
    if (!net_sites_save(app->remote_sites, &err))
        snprintf(app->remote_note, sizeof(app->remote_note), "%s", err.c_str());
}

// A remote pane is a second filesystem seen over the network. Anything that
// needs a real local file says so here, rather than letting a local call fail on
// a path that only the transport understands.
static bool remote_blocked(AppPanes* app, const Pane& pane, const char* what) {
    if (!in_remote(pane)) return false;
    snprintf(app->status_msg, sizeof(app->status_msg),
             "%s needs a local file: F5 downloads from a site", what);
    return true;
}

// The login a remote pane needs. A pane can arrive at a site from a restored
// session, the site manager, or a typed command line, and in every case the site
// list is the only place the password lives. When it is not there, the status
// line says what to do rather than leaving the operation to fail silently.
static const NetSite* remote_site_for_root(AppPanes* app, const std::string& root) {
    const NetSite* site = net_site_for(root);
    if (!site)
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "No login for %s: Ctrl+F edits the saved sites, Ctrl+N connects again",
                 root.c_str());
    return site;
}

static const NetSite* remote_site_of(AppPanes* app, const Pane& pane) {
    return remote_site_for_root(app, pane.remote_root);
}

// The server-side path of a row in a remote pane: the pane's path plus the row's
// name. Remote rows carry no `rel` of their own, unlike branch and archive rows.
static std::string remote_row_path(const Pane& pane, const char* name) {
    return net_join_remote(pane.remote_path, name);
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

// Build the rows for one directory level inside an archive. arch_list reports
// every member with its full path, so the rows are the members that sit at
// `prefix` plus exactly one component: a folder inside the archive becomes an
// ordinary row and can be walked into. `rel` is filled parallel to the entries
// with each member's path inside the archive, which is what entry_rel and the
// virtual path need.
struct ArchiveRow {
    FileEntry* fe;
    std::string rel;
};

static void list_archive_dir(const char* archive, const char* prefix,
                             FileEntry*** out_entries, int* out_count,
                             StringList* rel) {
    *out_entries = nullptr;
    *out_count = 0;

    std::vector<ArchEntry> members;
    std::string err;
    if (!arch_list(archive, &members, &err)) return;

    size_t plen = prefix ? strlen(prefix) : 0;
    std::vector<ArchiveRow> rows;
    for (const ArchEntry& m : members) {
        const char* r = m.rel;
        if (plen) {
            if (strncmp(r, prefix, plen) != 0 || r[plen] != '/') continue;
            r += plen + 1;
        }
        if (!*r) continue;                     // the level itself
        if (strchr(r, '/')) continue;          // deeper levels get their own row
        if (!fs_get_show_hidden() && r[0] == '.') continue;
        const char* base = strrchr(r, '/');
        base = base ? base + 1 : r;
        FileEntry* fe = file_entry_new(base, m.mode, m.size, m.mtime);
        if (!fe) continue;
        rows.push_back({ fe, std::string(r) });
    }

    // Same order a real directory would use, so the cursor and the sort keys
    // behave identically inside an archive.
    std::sort(rows.begin(), rows.end(), [](const ArchiveRow& a, const ArchiveRow& b) {
        const FileEntry* pa = a.fe;
        const FileEntry* pb = b.fe;
        return compare_files(&pa, &pb) < 0;
    });

    // ".." is always a row. Inside an archive it steps out one level; at the
    // archive root it leaves the archive and lands on the directory holding the
    // archive file. Without that row the only way back out is Backspace, which
    // is invisible to anyone who has not read the help.
    const int lead = 1;
    *out_entries = (FileEntry**)calloc(rows.size() + lead, sizeof(FileEntry*));
    if (!*out_entries) {
        for (const ArchiveRow& r : rows) file_entry_free(r.fe);
        return;
    }
    int i = 0;
    if (lead) {
        (*out_entries)[0] = file_entry_new("..", S_IFDIR | 0755, 0, 0);
        rel->push_back(std::string());         // ".." is resolved by dirname()
        i = 1;
    }
    for (const ArchiveRow& r : rows) { (*out_entries)[i] = r.fe; rel->push_back(r.rel); i++; }
    *out_count = i;
}

void refresh_pane(Pane* pane, const char* path) {
    bool dir_changed = strcmp(pane->current_dir, path) != 0;
    char previous[4096];
    snprintf(previous, sizeof(previous), "%s", pane->current_dir);
    free_entries(pane->entries, pane->entry_count);
    // Nothing is a listing until one of the three branches below puts one there.
    // A pane that walks onto a site with no saved login gets no listing at all,
    // and rows that have been freed are only safe to leave behind with a count of
    // zero to reach them.
    pane->entries = nullptr;
    pane->entry_count = 0;
    strncpy(pane->current_dir, path, sizeof(pane->current_dir) - 1);
    pane->current_dir[sizeof(pane->current_dir) - 1] = '\0';
    pane->selected = 0;
    pane->anchor = 0;
    pane->rel.clear();
    char arch[4096], prefix[1024];
    std::string nroot, nrpath;
    if (net_split_virtual(path, &nroot, &nrpath)) {
        const NetSite* site = net_site_for(nroot);
        std::string err;
        if (site) {
            list_remote_dir(*site, nrpath, &pane->entries, &pane->entry_count, &err);
        } else {
            err = "No login for " + nroot + ": Ctrl+N connects to a URL, Ctrl+F edits the saved sites";
        }
        if (!err.empty()) g_net_note = err;
        snprintf(pane->remote_root, sizeof(pane->remote_root), "%s", nroot.c_str());
        snprintf(pane->remote_path, sizeof(pane->remote_path), "%s", nrpath.c_str());
        // Remember where the pane came from, because ".." at the site root has
        // to land somewhere real. Only on the step in: walking deeper inside the
        // site must not overwrite the way back.
        if (dir_changed && previous[0] && !net_is_remote(previous))
            snprintf(pane->remote_came_from, sizeof(pane->remote_came_from), "%s", previous);
        pane->archive_path[0] = '\0';
        pane->archive_prefix[0] = '\0';
        pane->list_mode = LIST_REMOTE;
    } else if (arch_split_virtual(path, arch, sizeof arch, prefix, sizeof prefix)) {
        list_archive_dir(arch, prefix, &pane->entries, &pane->entry_count, &pane->rel);
        snprintf(pane->archive_path, sizeof pane->archive_path, "%s", arch);
        snprintf(pane->archive_prefix, sizeof pane->archive_prefix, "%s", prefix);
        pane->remote_root[0] = '\0';
        pane->remote_path[0] = '\0';
        pane->list_mode = LIST_ARCHIVE;
    } else {
        list_directory(path, &pane->entries, &pane->entry_count);
        pane->archive_path[0] = '\0';
        pane->archive_prefix[0] = '\0';
        pane->remote_root[0] = '\0';
        pane->remote_path[0] = '\0';
        pane->remote_came_from[0] = '\0';
        // A plain navigation also ends branch view.
        pane->list_mode = LIST_NORMAL;
    }
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
    rebuild_view(*pane);
}

// TC: Space on an unmarked directory sums its contents so the size column can
// show a real number instead of "-". Alt+Shift+Enter does it for every dir.
static void compute_dir_size(Pane& pane, int idx) {
    if (idx < 0 || idx >= pane.entry_count || idx >= (int)pane.dir_size.size()) return;
    if (in_archive(pane)) return;   // a member's packed size is not a tree total
    if (in_remote(pane)) return;    // summing a remote tree means downloading it
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
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", pane->current_dir, entry_rel(*pane, pane->selected));
    // TC opens an archive the way it opens a directory, so Ctrl+Up on an
    // archive file opens the archive in a new tab.
    bool openable = is_directory(e->name, e->mode) || arch_is_archive(full);
    if (strcmp(e->name, "..") == 0 || !openable) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Not a directory: %s", e->name);
        return;
    }
    tab_new(pane, activate);
    navigate_to(app, pane, full);
}

// TC Ctrl+Left/Right: show the directory under the cursor in the other pane.
static void open_cursor_in_other_pane(AppPanes* app, Pane* src, Pane* dst) {
    if (src->selected < 0 || src->selected >= src->entry_count) return;
    FileEntry* e = src->entries[src->selected];
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", src->current_dir, entry_rel(*src, src->selected));
    bool openable = is_directory(e->name, e->mode) || arch_is_archive(full);
    if (strcmp(e->name, "..") == 0 || !openable) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Not a directory: %s", e->name);
        return;
    }
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
    if (archive_readonly(app, active)) return;   // a virtual path has nothing to walk
    if (in_remote(active)) {
        // Searching a site would mean listing every directory on the server, one
        // round trip each. F5 takes a tree down, and then the local search runs.
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Search needs a local directory: F5 downloads from a site first");
        return;
    }
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

// Defined with the remote dialogs. "cd ftp://..." on the command line and the
// Ctrl+N field both end up there, so a typed address means one thing however it
// was entered.
static void remote_open_url(AppPanes* app, const char* url);

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
        } else if (net_is_remote(arg)) {
            // A URL is a directory too, just not one the relative-path rules can
            // join onto the folder the pane is in.
            snprintf(target, sizeof(target), "%s", arg);
        } else if (arg[0] == '/') {
            snprintf(target, sizeof(target), "%s", arg);
        } else {
            snprintf(target, sizeof(target), "%s/%s", active.current_dir, arg);
        }
        if (net_is_remote(target)) {
            remote_open_url(app, target);
        } else {
            DIR* probe = opendir(target);
            if (probe) {
                closedir(probe);
                navigate_to(app, &active, target);
            } else {
                snprintf(app->status_msg, sizeof(app->status_msg), "cd: no such directory: %s", target);
            }
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
static void prog_begin(AppPanes* app, const char* src, const char* dst, bool is_move);
// Defined with the keyboard handlers; the deferred-action table and the Commands
// menu both need them earlier.
static void do_copy_in_place(AppPanes* app);
// Defined with the archive viewer helpers; F5 inside an archive needs it to turn
// a row's display path into the member name the tool wrote.
static std::string member_raw(const Pane& pane, const char* rel);
static void do_symlink_other(AppPanes* app);
static void count_marked_sizes(AppPanes* app);
static void fill_dir_sizes(AppPanes* app);

// A remote delete goes over the transport. A server has no trash, so the trash
// setting is not honoured here and the prompt says so before any of it happens.
static void do_delete_remote(AppPanes* app, Pane& pane) {
    const NetSite* site = remote_site_of(app, pane);
    if (!site) return;
    prog_begin(app, pane.current_dir, pane.current_dir, false);
    app->prog_is_delete = true;
    int ok = 0;
    int marks = marked_count(pane);
    std::string err;
    for (int i = 0; i < pane.entry_count; i++) {
        if (marks == 0 && i != pane.selected) continue;
        if (marks > 0 && !pane.marked[i]) continue;
        FileEntry* e = pane.entries[i];
        if (strcmp(e->name, "..") == 0) continue;
        std::string rpath = remote_row_path(pane, e->name);
        if (net_remove_tree(*site, rpath, copy_progress_cb, app, &err)) ok++;
        if (app->quit_request || app->prog_abort) break;
    }
    app->prog_active = false;
    if (app->prog_abort && !ok) snprintf(app->status_msg, sizeof(app->status_msg), "Stopped: nothing deleted");
    else if (ok) snprintf(app->status_msg, sizeof(app->status_msg), "Deleted %d entries on the server%s", ok, app->prog_abort ? " (aborted)" : "");
    else    snprintf(app->status_msg, sizeof(app->status_msg), "Delete failed: %s", err.c_str());
    refresh_pane(&pane, pane.current_dir);
}

static void do_delete(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (in_remote(active)) { do_delete_remote(app, active); return; }
    bool to_trash = app->delete_trash != 0;
    prog_begin(app, active.current_dir, active.current_dir, false);
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
        if (app->quit_request || app->prog_abort) break;
    }
    app->prog_active = false;
    if (app->prog_abort && !ok) snprintf(app->status_msg, sizeof(app->status_msg), "Stopped: nothing deleted");
    else if (ok) snprintf(app->status_msg, sizeof(app->status_msg), to_trash ? "Trashed %d entries" : "Deleted %d entries", ok);
    else    snprintf(app->status_msg, sizeof(app->status_msg), "Delete failed: %s", strerror(errno));
    refresh_pane(&active, active.current_dir);
}

static void request_delete(AppPanes* app, bool force_direct) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (branch_blocked(app, active) || archive_readonly(app, active)) return;
    int n = count_targets(app, active);
    if (n == 0) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to delete");
        return;
    }
    // TC splits "direkt" and "Papierkorb": Shift always deletes for good, and
    // the prompt has to say which one is about to happen.
    if (in_remote(active)) {
        // There is no trash on a server, so the only question is whether the user
        // really means the permanent kind. The wording says so rather than
        // reusing the local prompt and letting them assume otherwise.
        app->delete_trash = 0;
        app->confirm = CONF_DELETE;
        snprintf(app->confirm_text, sizeof(app->confirm_text),
                 "Delete %d entrie%s from %s ? the server has no trash",
                 n, n == 1 ? "y" : "s", active.current_dir);
        return;
    }
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

// The window's framebuffer scale: device pixels per ImGui logical pixel. ImGui
// rasterizes glyphs at this density, so a 2x display gets a 26 px raster of a
// 13 px font instead of a 13 px raster stretched over 26 - which is the whole
// difference between a crisp and a blurry high-DPI window. It changes when the
// window moves to a different monitor or the desktop scaling changes, and the
// atlas has to be rebuilt for the new density, which is why a change sets
// font_dirty rather than rebuilding here.
bool app_panes_set_display_scale(AppPanes* app, float scale) {
    if (!(scale > 0.0f)) scale = 1.0f;
    if (scale == app->display_scale) return false;
    app->display_scale = scale;
    app->font_dirty = true;
    return true;
}

// ---------------------------------------------------------------------------
// Identity, title bar, fonts
//
// All of it is plain functions over buffers rather than widgets, so the strings
// a person judges by eye can be checked headless: what the app calls itself, and
// what the window manager draws, are the two places a wrong name shows up.
// ---------------------------------------------------------------------------

// "immc 0.9.0 (mc-clone)". The About window, --version and the title bar start
// from here, so they cannot drift apart.
void app_identity_line(char* out, size_t n) {
    snprintf(out, n, "%s %s (%s)", MC_APP_NAME, MC_VERSION, MC_APP_BINARY);
}

// SDL reports its version as one packed integer; the About window and --version
// show the three numbers a person can look up.
static void sdl_version_string(char* out, size_t n) {
    int v = SDL_GetVersion();
    snprintf(out, n, "%d.%d.%d", SDL_VERSIONNUM_MAJOR(v),
             SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
}

// The About window's header: what the app is, what it was built against, where
// its settings live, and which archive tools this machine actually has. The last
// line is the only one that can be false on a given install, so it is measured
// instead of asserted.
void app_about_identity(char* out, size_t n) {
    char cfgp[1024];
    mc_config_path(cfgp, sizeof(cfgp));

    static const ArchKind kShown[] = { ARCH_ZIP, ARCH_TAR, ARCH_7Z };
    char tools[256];
    tools[0] = '\0';
    for (ArchKind k : kShown) {
        if (!arch_tool_available(k)) continue;
        if (tools[0]) strncat(tools, ", ", sizeof(tools) - strlen(tools) - 1);
        strncat(tools, arch_kind_name(k), sizeof(tools) - strlen(tools) - 1);
    }
    if (!tools[0]) snprintf(tools, sizeof(tools), "none installed");

    char sdlv[32];
    sdl_version_string(sdlv, sizeof(sdlv));

    char raster[64];
    mc_font_rasterizer(raster, sizeof(raster));

    snprintf(out, n,
             "%s %s - a Total Commander style file manager\n"
             "executable %s - Dear ImGui %s, SDL %s\n"
             "settings %s\n"
             "text rasterized with %s\n"
             "archive tools found: %s%s\n",
             MC_APP_NAME, MC_VERSION, MC_APP_BINARY, IMGUI_VERSION, sdlv, cfgp, raster,
             tools, arch_tool_available(ARCH_RAR) ? ", RAR read-only" : "");
}

// Past this many characters a title bar stops being readable, and the path is
// the part worth shortening - never the app name in front of it.
static const size_t MC_TITLE_MAX = 100;

// Keep the head of a path and its last segment, dropping what is between them.
// A title that elides from the front hides which archive or volume the pane is
// in, and one that elides from the back hides where it is.
static void elide_path_chars(const char* path, size_t max, char* out, size_t n) {
    if (strlen(path) <= max) { snprintf(out, n, "%s", path); return; }
    if (max < 8) { snprintf(out, n, "%s", path + (strlen(path) > max ? strlen(path) - max : 0)); return; }
    const char* slash = strrchr(path, '/');
    const char* leaf  = (slash && slash[1]) ? slash + 1 : path;
    size_t leaf_len = strlen(leaf);
    // A leaf longer than the whole budget (a deeply nested archive member) still
    // gets the tail, which is the only part that identifies it.
    if (leaf_len + 3 >= max) {
        snprintf(out, n, "...%s", path + (strlen(path) + 3 > max ? strlen(path) - (max - 3) : 0));
        return;
    }
    snprintf(out, n, "%.*s...%s", (int)(max - leaf_len - 3), path, leaf);
}

// Title bar text: the app name and what the active pane is looking at. Inside an
// archive current_dir is the pane's virtual path, so the title names the archive
// as well as the level inside it.
void app_window_title(AppPanes* app, char* out, size_t n) {
    const Pane& active = app->left_focus ? app->left : app->right;
    char path[MC_TITLE_MAX + 8];
    elide_path_chars(active.current_dir, MC_TITLE_MAX, path, sizeof(path));
    snprintf(out, n, "%s - %s", MC_APP_NAME, path);
}

// The places fonts live. Nothing is configured: a ttf dropped into the user's
// own font directory shows up in the picker without an edit to the config file.
static const char* const kFontDirs[] = {
    "/usr/share/fonts", "/usr/local/share/fonts",
};

static bool font_file_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    return !strcasecmp(dot, ".ttf") || !strcasecmp(dot, ".otf") ||
           !strcasecmp(dot, ".ttc") || !strcasecmp(dot, ".otc");
}

// Recursive, depth- and count-capped. A system font tree holds thousands of
// files and the picker shows one scrollable list, so the cap is the design.
static void scan_font_tree(const char* root, StringList& out, int depth, int max) {
    if (depth < 0 || (int)out.size() >= max) return;
    DIR* d = opendir(root);
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;   // also skips . and ..
        std::string full = std::string(root) + "/" + de->d_name;
        struct stat st;
        if (lstat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_font_tree(full.c_str(), out, depth - 1, max);
        } else if (S_ISREG(st.st_mode) && font_file_ext(de->d_name)) {
            bool dup = false;
            for (const std::string& s : out) if (s == full) { dup = true; break; }
            if (!dup) out.push_back(full);
        }
        if ((int)out.size() >= max) break;
    }
    closedir(d);
}

// The standard places, then the list sorted by file name so the members of one
// family sit together instead of wherever the walk reached them.
void app_scan_fonts(StringList& out) {
    out.clear();
    std::vector<std::string> dirs;
    for (const char* d : kFontDirs) dirs.push_back(d);
    const char* home = getenv("HOME");
    if (home && *home) {
        dirs.push_back(std::string(home) + "/.local/share/fonts");
        dirs.push_back(std::string(home) + "/.fonts");
    }
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) dirs.push_back(std::string(xdg) + "/fonts");

    for (const std::string& d : dirs) scan_font_tree(d.c_str(), out, 6, 400);

    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const char* ba = strrchr(a.c_str(), '/'); ba = ba ? ba + 1 : a.c_str();
        const char* bb = strrchr(b.c_str(), '/'); bb = bb ? bb + 1 : b.c_str();
        int c = strcasecmp(ba, bb);
        return c ? c < 0 : a < b;
    });
}

// "DejaVuSansMono.ttf" for a path: the picker lists names and tooltips the paths.
static const char* file_base(const char* path) {
    const char* slash = strrchr(path, '/');
    return (slash && slash[1]) ? slash + 1 : path;
}

// ImGui's font loader asserts instead of failing when the bytes are not a font,
// so the file is sniffed before it is handed over: the signatures stb_truetype
// understands, and nothing else. A config or a filter box can name any file.
static bool font_file_looks_readable(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    unsigned char magic[4] = { 0, 0, 0, 0 };
    size_t got = fread(magic, 1, 4, f);
    fclose(f);
    if (got < 4) return false;
    if (!memcmp(magic, "\0\x01\0\0", 4)) return true;   // TrueType
    if (!memcmp(magic, "true", 4))       return true;   // TrueType, Apple spelling
    if (!memcmp(magic, "ttcf", 4))       return true;   // TrueType collection
    if (!memcmp(magic, "OTTO", 4))       return true;   // OpenType / CFF
    if (!memcmp(magic, "O\0\0\0", 4))    return true;   // OpenType, older spelling
    return false;
}

// ---------------------------------------------------------------------------
// Hinting
//
// A glyph outline is a curve; hinting is what decides which pixels it lights up.
// It is the difference between a stem that lands on one pixel and one that lights
// two pixels half bright, so at the small monospace sizes a file manager lives on
// it is the difference between legible and grey. The modes are named, and the
// names are what the config stores - the same arrangement the theme uses - so a
// hand-edited config says what the Options dialog shows.
//
// The bits below are this app's own; mc_freetype_loader_flags() translates them
// for the loader. That keeps one table that compiles in a build without
// FreeType, where there is a vocabulary to remember but nothing to honour it.
// ---------------------------------------------------------------------------

enum MCHintingBit {
    MC_HINT_NONE      = 1 << 0,   // leave the outline alone
    MC_HINT_LIGHT     = 1 << 1,   // snap vertically only
    MC_HINT_STRONG    = 1 << 2,   // snap both ways
    MC_HINT_NO_SMOOTH = 1 << 3,   // no grey: monochrome coverage
};

struct MCHintingMode {
    const char* name;   // the config's value and the combo's label
    int         bits;
    const char* note;   // one line in the dialog: what picking it costs
};

static const MCHintingMode kHintingModes[] = {
    { "default",  0,
      "the font's own hinter at normal strength: what the loader chooses for this font" },
    { "none",     MC_HINT_NONE,
      "no hinting at all: the outline is lit as drawn, so stems land between pixels and come out grey "
      "- which is what the built-in loader does whatever this says" },
    { "light",    MC_HINT_LIGHT,
      "snapped to the grid vertically only: keeps the spacing between glyphs, softer across them" },
    { "mono",     MC_HINT_STRONG,
      "snapped both ways: the crispest at small monospace sizes, at the cost of even spacing" },
    { "mono (no smoothing)", MC_HINT_STRONG | MC_HINT_NO_SMOOTH,
      "snapped both ways with no grey at all: hard edges, and thin strokes can break up at small sizes" },
};

static const int kHintingCount = (int)(sizeof(kHintingModes) / sizeof(kHintingModes[0]));

const char* const* mc_hinting_list(int* count) {
    static const char* names[kHintingCount];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < kHintingCount; i++) names[i] = kHintingModes[i].name;
        built = true;
    }
    *count = kHintingCount;
    return names;
}

// -1 for a name this build does not know, so a config edited by hand is either
// honoured or visibly ignored, never quietly reinterpreted.
int mc_hinting_index(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < kHintingCount; i++)
        if (!strcmp(kHintingModes[i].name, name)) return i;
    return -1;
}

// The dialog offers the choice only where something can carry it out.
bool mc_font_hinting_available() {
#ifdef MC_HAVE_FREETYPE
    return true;
#else
    return false;
#endif
}

// What rasterizes the text in this build. Worth showing rather than asserting:
// "the font is blurry" has two entirely different causes, and this line tells
// them apart.
void mc_font_rasterizer(char* out, size_t n) {
#ifdef MC_HAVE_FREETYPE
#if defined(FREETYPE_MAJOR)
    snprintf(out, n, "FreeType %d.%d.%d", FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
#elif defined(FT_MAJOR)
    snprintf(out, n, "FreeType %d.%d.%d", FT_MAJOR, FT_MINOR, FT_PATCH);
#else
    snprintf(out, n, "FreeType");
#endif
#else
    snprintf(out, n, "stb_truetype");
#endif
}

// One line about a mode, for the dialog to say what picking it costs.
static const char* mc_hinting_note(int index) {
    if (index < 0 || index >= kHintingCount) index = 0;
    return kHintingModes[index].note;
}

static unsigned int mc_freetype_loader_flags(const char* name) {
    int i = mc_hinting_index(name);
    if (i < 0) i = 0;                       // unknown or empty: the loader's own choice
    unsigned int bits = (unsigned int)kHintingModes[i].bits;
    unsigned int flags = 0;
    if (bits & MC_HINT_NONE)      flags |= ImGuiFreeTypeLoaderFlags_NoHinting;
    if (bits & MC_HINT_LIGHT)     flags |= ImGuiFreeTypeLoaderFlags_LightHinting;
    if (bits & MC_HINT_STRONG)    flags |= ImGuiFreeTypeLoaderFlags_MonoHinting;
    if (bits & MC_HINT_NO_SMOOTH) flags |= ImGuiFreeTypeLoaderFlags_Monochrome;
    return flags;
}

// Device pixels per logical pixel, from the two sizes SDL reports for one window.
// This is the density the glyph atlas gets baked at: ImGui takes its pixel density
// from io.DisplayFramebufferScale.x, and the SDL3 backend fills that field with
// exactly this ratio, so the number measured here is the number the font system
// ends up using. A measurement you cannot divide by - a minimized window reports
// no logical size, and a mode change reports the two sizes out of step for a
// frame - falls back to 1x rather than to a scale that would shrink text to
// nothing. The vertical ratio is usually the same number; where it is not, the
// backend still sets both components every frame, and the glyphs follow x.
float mc_framebuffer_scale(int logical_w, int pixel_w) {
    if (logical_w <= 0 || pixel_w <= 0) return 1.0f;
    float s = (float)pixel_w / (float)logical_w;
    return s > 0.0f ? s : 1.0f;
}

// Rebuild the font atlas from a path, a size and a hinting mode. A file that
// cannot be read is neither fatal nor silent: the built-in font goes back in and
// the caller is told, which is the difference between a picker that looks broken
// and one that explains itself.
//
// The size does not go where it looks like it should. Since 1.92 ImGui bakes
// glyphs at the size the *style* asks for - FontSizeBase * FontScaleMain - and the
// size handed to AddFont() only becomes that font's default, which ImGui reads
// once, on the first frame, to seed FontSizeBase. Passing the user's size to
// AddFont() therefore sets the size at startup and never again: the slider moves,
// the atlas is rebuilt, and the text stays exactly as big as it was. So every font
// here is added at one reference size and the requested size is carried by
// FontScaleMain, which is read on every frame. 13 px is not an arbitrary pick: it
// is the size AddFontDefault() uses for both of its built-in fonts, so the
// reference and the seeded base are the same number whichever font is loaded.
bool app_font_apply(const char* path, float size, const char* hinting) {
    const float kRefSize = 13.0f;
    ImFontAtlas* fonts = ImGui::GetIO().Fonts;
    ImGui::GetStyle().FontScaleMain = size > 0.0f ? size / kRefSize : 1.0f;
    fonts->ClearFonts();
#ifdef MC_HAVE_FREETYPE
    // The loader and its flags belong to the atlas, not to one font, and the
    // flags are read when a glyph is baked - so the atlas has to be cleared and
    // rebuilt for a hinting change to show, which is what this function is for.
    fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
    fonts->FontLoaderFlags = mc_freetype_loader_flags(hinting);
#else
    (void)hinting;   // remembered in the settings, nothing here can act on it
#endif
    if (path && path[0]) {
        // ImGui also asserts on a buffer under 100 bytes, so a truncated or
        // placeholder .ttf is turned down on size as well as on signature.
        struct stat st;
        bool readable = !stat(path, &st) && S_ISREG(st.st_mode) && st.st_size > 100 &&
                        font_file_looks_readable(path);
        // The reference size, not the requested one: the loader bakes at the size
        // the style asks for, so a 48 px request is rasterized by FreeType at 48 px
        // whatever this says.
        if (readable && fonts->AddFontFromFileTTF(path, kRefSize)) return true;
        fonts->ClearFonts();
        fonts->AddFontDefault();
        return false;
    }
    // AddFontDefault() chooses between its bitmap face and its scalable one by the
    // size it expects to reach the screen, which is why the scale is set before
    // this call and not after: at 20 px the scalable face is the one worth having.
    fonts->AddFontDefault();
    return true;
}

// The dialog only marks a pick; the atlas is rebuilt here, between frames, where
// ImGui allows it. A rejected file is dropped from the app's state so the config
// cannot end up naming a font that has never worked.
void app_panes_apply_font(AppPanes* app) {
    app->font_dirty = false;
    char size_note[64];
    if (app->font_size > 0.0f) snprintf(size_note, sizeof(size_note), " at %.0f px", app->font_size);
    else                       size_note[0] = '\0';

    // The mode is part of the sentence because it is part of the result: the same
    // file at the same size looks different under each of them, and a build that
    // cannot hint has to say so rather than name a mode it ignored.
    char hint_note[64];
    if (mc_font_hinting_available())
        snprintf(hint_note, sizeof(hint_note), ", %s hinting", app->hinting);
    else
        snprintf(hint_note, sizeof(hint_note), "%s", "");

    if (app_font_apply(app->font, app->font_size, app->hinting)) {
        snprintf(app->font_msg, sizeof(app->font_msg), "Font: %s%s%s",
                 app->font[0] ? file_base(app->font) : "built-in", size_note, hint_note);
    } else {
        snprintf(app->font_msg, sizeof(app->font_msg),
                 "\"%s\" could not be read as a font; the built-in font is in use",
                 file_base(app->font));
        app->font[0] = '\0';
    }
}

// ---------------------------------------------------------------------------
// Copy/move progress dialog. The copy runs synchronously; between files we
// pump a minimal ImGui frame that shows only this dialog, so the window
// stays responsive and shows live numbers.
// ---------------------------------------------------------------------------

static void render_progress_dialog(AppPanes* app) {
    if (!app->prog_active) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 sz(560, 270);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f,
                                   vp->Pos.y + (vp->Size.y - sz.y) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    const char* title = app->prog_is_delete ? "Deleting"
                      : (app->prog_is_move  ? "Moving" : "Copying");
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);

    ImGui::Text("%s", app->prog_src);
    // A transfer that is running on its own thread reports through its row. The
    // dialog reads the same atomics the list under the panes reads, which is why
    // one of them can stop reading without disturbing the other.
    if (app->prog_watch) {
        app->prog_done        = app->prog_watch->done.load();
        app->prog_total       = app->prog_watch->total.load();
        app->prog_files_done  = app->prog_watch->files_done.load();
        app->prog_files_total = app->prog_watch->files_total.load();
    }
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

    // Abort is a request, not a stop: the copy and the transport both poll the
    // progress callback, which returns false while this is set, so the operation
    // unwinds after the file it is on rather than mid-write. The pumped frame
    // loop keeps redrawing this dialog, so the button is live throughout.
    ImGui::Spacing();
    if (ImGui::Button("Abort")) {
        app->prog_abort = true;
        if (app->prog_watch) app->prog_watch->abort = true;
    }
    // Only a transfer that is already on a thread of its own can be handed over,
    // and the handover asks nothing of the transfer: the row under the panes has
    // the numbers, the Abort button and the finish already. That is why this can
    // be pressed at any moment, in the middle of a file, and why it is not
    // offered for an operation that is running on this thread - there the dialog
    // is not watching the copy, it is the copy.
    if (app->prog_watch) {
        ImGui::SameLine();
        if (ImGui::Button("Run in background")) app->prog_handoff = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The transfer goes on, and the list under the panes carries it.\n"
                              "Nothing about the transfer itself changes.");
    }
    if (app->prog_abort) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "stopping after the current file...");
    } else {
        ImGui::TextDisabled("Abort stops the operation. What is already transferred stays.");
    }
    ImGui::End();
}

static void pump_progress_ui(AppPanes* app) {
    // The progress dialog is drawn into the main window, so without one there is
    // nothing to pump. The operation still runs; it just has no UI to abort.
    if (!app->sdl_window) return;
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
    return !app->quit_request && !app->prog_abort;   // closing the window, or Abort
}

static void prog_begin(AppPanes* app, const char* src, const char* dst, bool is_move) {
    app->prog_active = true;
    app->prog_abort = false;
    app->prog_is_move = is_move;
    app->prog_is_delete = false;
    snprintf(app->prog_src, sizeof(app->prog_src), "%s", src);
    snprintf(app->prog_dst, sizeof(app->prog_dst), "%s", dst);
    app->prog_done = 0;
    app->prog_total = 0;
    app->prog_files_done = 0;
    app->prog_files_total = 0;
    app->prog_t0 = SDL_GetTicks() / 1000.0;
    app->prog_last_pump = 0.0;
}

// A copy that crosses a network is on its own thread whether or not the user
// asked for the background, so the foreground form of it is a window onto that
// thread rather than a copy running inside the UI. That is the whole reason the
// progress dialog can offer "Run in background" at any moment, in the middle of
// a file: handing over changes who is watching, not what is running.
static void prog_begin_watch(AppPanes* app, BgTransfer* t) {
    prog_begin(app, t->src.c_str(), t->dst.c_str(), t->is_move);
    app->prog_watch   = t;
    app->prog_handoff = false;
    app->prog_t0      = t->t0;   // the transfer started before this dialog did
}

// One frame of watching: draw, breathe, and say whether the watching goes on.
// Kept apart from the loop so the handover can be checked without a display.
static bool watch_transfer_tick(AppPanes* app, BgTransfer* t) {
    pump_progress_ui(app);
    SDL_Delay(30);
    if (app->quit_request || app->prog_handoff) return false;
    return t->state.load(std::memory_order_acquire) == BgTransfer::BG_RUNNING;
}

// Watch it here, in this dialog, until it is over or the user decides otherwise.
// Either way the row stays in the transfer list: it has been the transfer's
// report all along, and it goes on being one after the dialog closes.
static void watch_remote_transfer(AppPanes* app, BgTransfer* t) {
    prog_begin_watch(app, t);
    while (watch_transfer_tick(app, t)) {}
    app->prog_active = false;
    app->prog_watch  = nullptr;
    if (app->prog_handoff)
        snprintf(app->status_msg, sizeof(app->status_msg), "Running in the background: %s -> %s",
                 t->src.c_str(), t->dst.c_str());
}

// ---------------------------------------------------------------------------
// Archives
// ---------------------------------------------------------------------------

// The archive an operation applies to: the one the pane is inside, or the
// archive file under the cursor.
static bool archive_target(AppPanes* app, const Pane& pane, char* out, size_t cap) {
    if (in_remote(pane)) {
        // The archive is bytes on a server, and the archive tools read files.
        // Fetching it and browsing it in place is the honest next step; until
        // then the status line says what to do instead.
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "An archive on a site has to be downloaded first (F5)");
        return false;
    }
    if (in_archive(pane)) { snprintf(out, cap, "%s", pane.archive_path); return true; }
    if (pane.selected < 0 || pane.selected >= pane.entry_count) return false;
    std::string full = std::string(pane.current_dir) + "/" + entry_rel(pane, pane.selected);
    if (!arch_is_archive(full.c_str())) {
        snprintf(app->status_msg, sizeof(app->status_msg), "No archive under the cursor");
        return false;
    }
    if (!arch_tool_available(arch_kind_of(full.c_str()))) {
        snprintf(app->status_msg, sizeof(app->status_msg), "No tool installed for %s archives",
                 arch_kind_name(arch_kind_of(full.c_str())));
        return false;
    }
    snprintf(out, cap, "%s", full.c_str());
    return true;
}

// The real directory a pane stands in. Inside an archive that is the directory
// holding the archive file, which is where extracting there has to land.
static void pane_real_dir(const Pane& pane, char* out, size_t cap) {
    if (!in_archive(pane)) { snprintf(out, cap, "%s", pane.current_dir); return; }
    snprintf(out, cap, "%s", pane.archive_path);
    char* slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    else snprintf(out, cap, "/");
}

// The rows an operation applies to, named relative to the pane: everything
// marked, or the row under the cursor when nothing is marked. In archive mode
// these are member paths, which is exactly what the extract calls want.
static StringList target_rel(const Pane& pane) {
    StringList out;
    int marks = marked_count(pane);
    for (int i = 0; i < pane.entry_count; i++) {
        if (marks == 0 && i != pane.selected) continue;
        if (marks > 0 && !pane.marked[i]) continue;
        if (strcmp(pane.entries[i]->name, "..") == 0) continue;
        out.push_back(entry_rel(pane, i));
    }
    return out;
}

// Progress for an archive operation. The callback shape is the copy/move one,
// so the existing dialog, its rate line and its Abort apply unchanged.
static void arch_prog_begin(AppPanes* app, const char* src, const char* dst) {
    app->prog_active = true;
    app->prog_abort = false;
    app->prog_is_move = false;
    app->prog_is_delete = false;
    snprintf(app->prog_src, sizeof(app->prog_src), "%s", src);
    snprintf(app->prog_dst, sizeof(app->prog_dst), "%s", dst);
    app->prog_done = 0;
    app->prog_total = 0;
    app->prog_files_done = 0;
    app->prog_files_total = 0;
    app->prog_t0 = SDL_GetTicks() / 1000.0;
    app->prog_last_pump = 0.0;
}

// TC Alt+F5 / Alt+Shift+F5. Packing gets a dialog rather than a name in the op
// bar, because it has real choices: format, level, a password, volumes. The
// sources and the target directory are captured here so the dialog cannot be
// acting on something other than what was asked for.
static void do_pack(AppPanes* app, bool and_delete) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (branch_blocked(app, active) || archive_readonly(app, active)) return;
    StringList names = target_rel(active);
    if (names.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to pack");
        return;
    }
    bool any_tool = false;
    for (ArchKind k : arch_pack_kinds()) if (arch_can_pack(k)) { any_tool = true; break; }
    if (!any_tool) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Packing needs zip, tar or 7z installed");
        return;
    }
    // The dialog carries the choices, so it captures the sources and the target
    // directory now: marks and tabs cannot change underneath it.
    app->pack_names = names;
    snprintf(app->pack_dir, sizeof(app->pack_dir), "%s", active.current_dir);
    // Default name: the single source without its own suffix, else "archive".
    std::string base = names[0];
    size_t slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    snprintf(app->pack_name, sizeof(app->pack_name), "%s",
             names.size() == 1 ? base.c_str() : "archive");
    app->pack_delete = and_delete;
    // A password is not a preference: it must not sit in the dialog and end up
    // in the next archive the user packs.
    app->pack_password[0] = '\0';
    app->pack_open = true;
    snprintf(app->status_msg, sizeof(app->status_msg), "Pack %d entr%s",
             (int)names.size(), names.size() == 1 ? "y" : "ies");
}

// The format combo owns the suffix, so a half-typed name cannot disagree with
// the format it is packed in. Longer suffixes are tried first: arch_pack_kinds()
// lists ".tar.gz" before ".tar".
static std::string strip_archive_suffix(const std::string& s) {
    for (ArchKind k : arch_pack_kinds()) {
        std::string suf = arch_kind_suffix(k);
        if (s.size() > suf.size() &&
            s.compare(s.size() - suf.size(), suf.size(), suf) == 0)
            return s.substr(0, s.size() - suf.size());
    }
    return s;
}

// The dialog state as the backend understands it, so the preview in the dialog
// and the operation that runs on Pack can never disagree.
static ArchPackOpts pack_opts(const AppPanes* app) {
    ArchPackOpts o = arch_pack_opts_default(arch_pack_kinds()[app->pack_kind_idx]);
    if (app->pack_use_level) o.level = app->pack_level;
    o.password = app->pack_password;
    o.volume_bytes = app->pack_volume_bytes;
    return o;
}

// The dialog's Pack button, and Enter. Runs the tool with the progress bar the
// copy operations already use, so abort works the same way here.
static void pack_commit(AppPanes* app) {
    ArchKind kind = arch_pack_kinds()[app->pack_kind_idx];
    ArchPackOpts opts = pack_opts(app);

    std::string name = app->pack_name;
    while (!name.empty() && isspace((unsigned char)name.back())) name.pop_back();
    if (name.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Pack cancelled: the archive needs a name");
        app->pack_open = false;
        app->pack_names.clear();
        return;
    }
    std::string arch = std::string(app->pack_dir) + "/" +
                       strip_archive_suffix(name) + arch_kind_suffix(kind);

    std::string why;
    if (!arch_pack_supported(opts, &why)) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Cannot pack: %s", why.c_str());
        return;                                  // leave the dialog up to fix
    }
    if (!arch_can_pack(kind)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Cannot pack %s: %s is not installed", arch.c_str(), arch_kind_name(kind));
        return;
    }

    StringList names = app->pack_names;
    app->pack_names.clear();
    app->pack_open = false;

    arch_prog_begin(app, app->pack_dir, arch.c_str());
    std::string err;
    bool ok = arch_pack_opts(arch.c_str(), app->pack_dir, names, opts,
                             copy_progress_cb, app, &err);
    app->prog_active = false;
    if (!ok) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Pack failed: %s", err.c_str());
        return;
    }
    int removed = 0;
    // Alt+Shift+F5 is a move into the archive, so the sources go too.
    if (app->pack_delete) {
        for (const std::string& n : names) {
            std::string full = std::string(app->pack_dir) + "/" + n;
            if (fs_delete_entry_progress(full.c_str(), copy_progress_cb, app)) removed++;
        }
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "Packed %d entr%s into %s%s",
             (int)names.size(), names.size() == 1 ? "y" : "ies", arch.c_str(),
             removed ? ", removed the sources" : "");
    refresh_pane(&app->left, app->left.current_dir);
    refresh_pane(&app->right, app->right.current_dir);
}

// Volume sizes worth offering. 7z takes a byte count, so the labels are the
// only place units appear.
static const long long kPackVolumes[] = { 0, 1024LL, 1024LL * 1024,
                                          100LL * 1024 * 1024, 700LL * 1024 * 1024,
                                          1024LL * 1024 * 1024, 4LL * 1024 * 1024 * 1024 };
static const char* const kPackVolumeLabels[] = {
    "one file", "1 KB", "1 MB", "100 MB", "700 MB (CD)", "1 GB", "4 GB" };



// A path has no spaces, and ImGui's word wrap only breaks at spaces, so a long
// one is a single unbreakable word that runs off the window and gets clipped.
// Shorten it in the middle instead: both ends stay, which is what identifies a
// directory, and the line always fits.
//
// The kept characters are split unevenly, two to the head for one to the tail.
// The tail only has to be long enough to name the leaf, but the head has to say
// which project that leaf belongs to: "quarterly...2026" is guesswork where
// "/home/adam/docu...2026" is not. An even split gets neither right.
static void elide_split(int keep, int* head, int* tail) {
    *head = (keep * 2 + 2) / 3;
    *tail = keep - *head;
}

// Ask for the caret to land on the item submitted next. Claiming it keeps the
// command line's every-frame request from overwriting it later in the frame.
static void claim_focus(AppPanes* app) {
    ImGui::SetKeyboardFocusHere();
    app->focus_claimed = true;
}

static std::string elide_middle(const char* s, float max_w) {
    if (ImGui::CalcTextSize(s).x <= max_w) return s;
    const char* gap = "...";
    const int n = (int)strlen(s);
    // The candidate only gets narrower as fewer characters are kept - the split
    // does not change its length - so binary search for the largest split that
    // fits rather than walking every length.
    int lo = 1, hi = n - 1, best = 1;
    while (lo <= hi) {
        int keep = (lo + hi) / 2;
        int head, tail;
        elide_split(keep, &head, &tail);
        std::string cand = std::string(s, head) + gap + std::string(s + (n - tail));
        if (ImGui::CalcTextSize(cand.c_str()).x <= max_w) { best = keep; lo = keep + 1; }
        else hi = keep - 1;
    }
    int head, tail;
    elide_split(best, &head, &tail);
    return std::string(s, head) + gap + std::string(s + (n - tail));
}


// -FLT_MIN fills from the cursor to the right edge, but a labelled widget draws
// its label first and starts the frame after it, so the frame ends up past the
// edge by exactly the label's width. Fill to the edge with the label counted.
static void pack_fill(const char* label) {
    float label_w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - label_w);
}

// This ImGui does not wrap Text()/TextDisabled() at the window edge - only
// PushTextWrapPos()/TextWrapped() do - so a long sentence in a dialog runs
// straight past the frame and gets clipped. Every sentence in these dialogs goes
// through one of the two helpers below.
static void text_wrap_disabled(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushTextWrapPos(0.0f);          // 0 = wrap at the window's work rect
    ImGui::TextDisabledV(fmt, args);
    ImGui::PopTextWrapPos();
    va_end(args);
}

static void text_wrap_colored(ImVec4 c, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColoredV(c, fmt, args);
    ImGui::PopTextWrapPos();
    va_end(args);
}

static void render_pack_dialog(AppPanes* app) {
    if (!app->pack_open) return;
    const std::vector<ArchKind>& kinds = arch_pack_kinds();
    if (app->pack_kind_idx < 0 || app->pack_kind_idx >= (int)kinds.size())
        app->pack_kind_idx = 0;
    ArchKind kind = kinds[app->pack_kind_idx];

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(430, 330), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 215, io.DisplaySize.y * 0.5f - 165),
                            ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Pack into archive", &open, ImGuiWindowFlags_NoCollapse);

    char items[256] = { 0 };
    size_t used = 0;
    for (ArchKind k : kinds) {
        const char* n = arch_kind_name(k);
        size_t nlen = strlen(n);
        if (used + nlen + 2 >= sizeof(items)) break;
        memcpy(items + used, n, nlen + 1);
        used += nlen + 1;
    }
    items[used++] = '\0';

    pack_fill("Archive name");
    ImGui::InputText("Archive name", app->pack_name, sizeof(app->pack_name));
    // Two lines, each measured as it will be drawn: the file name and the
    // directory are both unbounded, and a path has no spaces for the wrap to
    // break on, so it is shortened in the middle rather than clipped.
    float avail = ImGui::GetContentRegionAvail().x;
    std::string written = "Written as " + strip_archive_suffix(app->pack_name) + arch_kind_suffix(kind);
    std::string into = std::string("into ") + app->pack_dir;
    text_wrap_disabled("%s", elide_middle(written.c_str(), avail).c_str());
    text_wrap_disabled("%s", elide_middle(into.c_str(), avail).c_str());

    pack_fill("Format");
    if (ImGui::Combo("Format", &app->pack_kind_idx, items)) {
        // A format that cannot honour a choice drops it, so the dialog never
        // shows a password box that would be ignored.
        ArchKind nk = kinds[app->pack_kind_idx];
        bool is_tar = nk == ARCH_TAR || nk == ARCH_TAR_GZ || nk == ARCH_TAR_BZ2 || nk == ARCH_TAR_XZ;
        if (is_tar) { app->pack_password[0] = '\0'; app->pack_volume_bytes = 0; }
        if (nk == ARCH_TAR) app->pack_use_level = false;
        if (nk != ARCH_7Z) app->pack_volume_bytes = 0;
    }

    bool is_tar = kind == ARCH_TAR || kind == ARCH_TAR_GZ ||
                  kind == ARCH_TAR_BZ2 || kind == ARCH_TAR_XZ;
    ImGui::BeginDisabled(kind == ARCH_TAR);
    ImGui::Checkbox("Compression level", &app->pack_use_level);
    if (kind == ARCH_TAR)
        text_wrap_disabled("a plain .tar is not compressed");
    else {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##level", &app->pack_level, 0, 9, "%d");
        text_wrap_disabled("0 store - 1 fastest - 6 the tool's default - 9 best");
    }
    ImGui::EndDisabled();

    if (is_tar) {
        text_wrap_disabled("tar cannot encrypt; pick zip or 7z for a password");
    } else {
        pack_fill("Password");
        ImGui::InputText("Password", app->pack_password, sizeof(app->pack_password),
                         ImGuiInputTextFlags_Password);
        text_wrap_disabled("the password is handed to %s, so it is visible in the "
                           "process list while packing", arch_kind_name(kind));
    }

    if (kind != ARCH_7Z) {
        text_wrap_disabled("only 7z writes volumes the tools can read back");
    } else {
        int vi = 0;
        for (int i = 0; i < (int)(sizeof(kPackVolumes) / sizeof(kPackVolumes[0])); i++)
            if (kPackVolumes[i] == app->pack_volume_bytes) vi = i;
        pack_fill("Volumes");
        if (ImGui::Combo("Volumes", &vi, kPackVolumeLabels,
                         (int)(sizeof(kPackVolumeLabels) / sizeof(kPackVolumeLabels[0]))))
            app->pack_volume_bytes = kPackVolumes[vi];
    }

    ImGui::Checkbox("Delete the sources afterwards", &app->pack_delete);

    std::string why;
    bool supported = arch_pack_supported(pack_opts(app), &why);
    ImGui::Separator();
    if (!arch_can_pack(kind)) {
        text_wrap_colored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s is not installed", arch_kind_name(kind));
    } else if (!supported) {
        text_wrap_colored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", why.c_str());
    }
    ImGui::Spacing();
    bool can_pack = arch_can_pack(kind) && supported && app->pack_name[0];
    ImGui::BeginDisabled(!can_pack);
    // The button only records the request: packing blocks and pumps its own
    // ImGui frames, which cannot happen while this dialog's frame is open.
    if (ImGui::Button("Pack")) app->pending_op = DEFER_PACK;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) { app->pack_open = false; app->pack_names.clear(); }
    text_wrap_disabled("Enter packs, Esc cancels");
    if (!open) { app->pack_open = false; app->pack_names.clear(); }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// The operation dialog: F5 copy, F6 move, F7 folder, Shift+F4 file, F2 rename
// ---------------------------------------------------------------------------

static const char* op_title(AppOp op) {
    switch (op) {
    case OP_COPY:    return "Copy";
    case OP_MOVE:    return "Move";
    case OP_RENAME:  return "Rename";
    case OP_NEWFILE: return "New file";
    case OP_MKDIR:   return "Create directory";
    default:         return "Operation";
    }
}

// The verb on the button is the operation, so the dialog reads as a sentence:
// the field above it is what gets copied, moved, renamed or created.
static const char* op_button_label(AppOp op) {
    switch (op) {
    case OP_COPY:    return "Copy";
    case OP_MOVE:    return "Move";
    case OP_RENAME:  return "Rename";
    case OP_NEWFILE: return "Create";
    case OP_MKDIR:   return "Create";
    default:         return "OK";
    }
}

// One dialog for all five operations, because they are the same shape: a source
// that is already decided, one name or destination to type, and a target
// directory that has to be visible while typing it. Copy and move add the
// destination as an editable path, which is what makes renaming or redirecting a
// copy possible instead of an all-or-nothing drop into the other pane.
// Only a transfer that crosses a network can be handed to a thread, so the
// dialog only offers the choice for one. The destination may still be sitting in
// the field rather than in the plan, so both are asked. A local copy is bounded by
// the disk, and a second thread writing into the same tree would buy nothing but
// a race.
static bool plan_crosses_network(const OpPlan& plan, const char* typed) {
    if (!plan.remote.empty()) return true;
    if (net_is_remote(plan.dst_dir.c_str())) return true;
    return typed && net_is_remote(typed);
}

static void render_op_dialog(AppPanes* app) {
    if (app->op == OP_NONE) return;
    ImGuiIO& io = ImGui::GetIO();
    const bool copy_move = app->op == OP_COPY || app->op == OP_MOVE;
    ImGui::SetNextWindowSize(ImVec2(620, copy_move ? 260 : 170), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 310, io.DisplaySize.y * 0.5f - 120),
                            ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin(op_title(app->op), &open, ImGuiWindowFlags_NoCollapse);

    const OpPlan& plan = app->op_plan;
    if (copy_move) {
        // What is being operated on, in full. A mark set that the user cannot
        // see would make the destination field a guess about unknown sources.
        ImGui::TextDisabled("From:");
        ImGui::SameLine();
        ImGui::TextUnformatted(elide_middle((plan.archive.empty() ? plan.src_dir : plan.archive).c_str(),
                                           ImGui::GetContentRegionAvail().x).c_str());
        std::string items;
        for (size_t i = 0; i < plan.names.size(); i++) {
            if (i >= 4) { items += "  (+" + std::to_string(plan.names.size() - i) + " more)"; break; }
            if (!items.empty()) items += ",  ";
            items += plan.names[i];
        }
        ImGui::TextDisabled("%d item%s:", (int)plan.names.size(), plan.names.size() == 1 ? "" : "s");
        ImGui::SameLine();
        text_wrap_disabled("%s", items.c_str());
        ImGui::Spacing();

        pack_fill("To:");
        if (app->op_focus_request) { claim_focus(app); app->op_focus_request = false; }
        ImGui::InputText("To:", app->op_name, sizeof(app->op_name));
        if (plan.archive.empty())
            text_wrap_disabled("End with / (or name a folder that exists) and every item keeps its "
                               "own name. With one item, the field is the target itself, so it can "
                               "also be a new name.");
        else
            text_wrap_disabled("The members come out under this folder with the paths they have "
                               "inside the archive. End with / to make the folder if it is missing.");
    } else {
        const std::string& in_dir = app->op_plan.src_dir.empty()
                                       ? (app->left_focus ? app->left : app->right).current_dir
                                       : app->op_plan.src_dir;
        if (app->op == OP_RENAME) {
            ImGui::TextDisabled("Renaming:");
            ImGui::SameLine();
            ImGui::TextUnformatted(app->op_plan.names.empty() ? "(nothing selected)"
                                                              : app->op_plan.names[0].c_str());
        }
        ImGui::TextDisabled("In:");
        ImGui::SameLine();
        ImGui::TextUnformatted(elide_middle(in_dir.c_str(), ImGui::GetContentRegionAvail().x).c_str());
        ImGui::Spacing();
        const char* field = app->op == OP_RENAME ? "New name:"
                          : app->op == OP_NEWFILE ? "File name:" : "Directory name:";
        pack_fill(field);
        if (app->op_focus_request) { claim_focus(app); app->op_focus_request = false; }
        ImGui::InputText(field, app->op_name, sizeof(app->op_name));
    }

    if (app->op_error[0]) {
        ImGui::Spacing();
        text_wrap_colored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", app->op_error);
    }

    ImGui::Spacing();
    // The button only records the request: copy and move block and pump their own
    // ImGui frames, which cannot happen while this dialog's frame is open.
    if (ImGui::Button(op_button_label(app->op))) app->pending_op = DEFER_OP_COMMIT;
    ImGui::SameLine();
    // The same operation, handed to a thread instead of run in front of the
    // panes. It commits exactly like the button above it and records the request
    // with the commit, because the choice has to survive the overwrite confirm
    // that may follow - which is why it lands in app->op_run_bg and not in a local.
    const bool network = copy_move && plan_crosses_network(plan, app->op_name);
    if (network) {
        if (ImGui::Button("Run in background")) {
            app->op_run_bg = true;
            app->pending_op = DEFER_OP_COMMIT;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The panes stay usable. The transfer is listed under them.");
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel")) app_panes_cancel_op(app);
    ImGui::SameLine();
    text_wrap_disabled("Enter runs it, Esc cancels.%s",
                       network ? "  Shift+Enter runs it in the background." : "");
    if (!open) app_panes_cancel_op(app);
    ImGui::End();
}

// TC Alt+F6 (into the other pane's directory) and Alt+F9 (into this one's).
// Inside an archive, "this one's" means the directory holding the archive.
static void do_extract(AppPanes* app, bool to_other_pane) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;
    char arch[4096], dest[4096];
    if (!archive_target(app, active, arch, sizeof(arch))) return;
    if (to_other_pane) {
        if (in_archive(other)) {
            snprintf(app->status_msg, sizeof(app->status_msg),
                     "The target pane is inside an archive");
            return;
        }
        if (in_remote(other)) {
            // Extraction writes many small files; one upload per file over a
            // network is a transfer the plan dialog already knows how to run.
            snprintf(app->status_msg, sizeof(app->status_msg),
                     "Extract into a local directory, then F5 it up to the site");
            return;
        }
        snprintf(dest, sizeof(dest), "%s", other.current_dir);
    } else {
        pane_real_dir(active, dest, sizeof(dest));
    }
    arch_prog_begin(app, arch, dest);
    std::string err;
    bool ok = arch_extract(arch, dest, copy_progress_cb, app, &err);
    app->prog_active = false;
    if (ok) snprintf(app->status_msg, sizeof(app->status_msg), "Extracted %s into %s", arch, dest);
    else snprintf(app->status_msg, sizeof(app->status_msg), "Extract failed: %s", err.c_str());
    refresh_pane(&active, active.current_dir);
    refresh_pane(&other, other.current_dir);
}

// TC Alt+Shift+F9: read every member and verify its CRC without writing anything.
static void do_archive_test(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    char arch[4096];
    if (!archive_target(app, active, arch, sizeof(arch))) return;
    arch_prog_begin(app, arch, "integrity test");
    std::string err;
    bool ok = arch_test(arch, copy_progress_cb, app, &err);
    app->prog_active = false;
    if (ok) snprintf(app->status_msg, sizeof(app->status_msg), "Archive OK: %s", arch);
    else snprintf(app->status_msg, sizeof(app->status_msg), "Archive test failed: %s", err.c_str());
}

// Paste out of an archive: the clipboard holds member paths, so pasting is an
// extraction of exactly those members.
static void do_paste_archive_now(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;
    const char* arch = app->clipboard.archive;
    std::vector<ArchEntry> members;
    std::string lerr;
    arch_list(arch, &members, &lerr);   // best effort: fall back to the display path
    std::vector<std::string> raws;
    for (const std::string& m : app->clipboard.paths) {
        const ArchEntry* e = arch_find(members, m.c_str());
        raws.push_back(e ? e->raw : m);
    }
    arch_prog_begin(app, arch, active.current_dir);
    std::string err;
    bool ok = arch_extract_members(arch, active.current_dir, raws, copy_progress_cb, app, &err);
    app->prog_active = false;
    if (ok) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Extracted %d member%s into %s",
                 (int)raws.size(), raws.size() == 1 ? "" : "s", active.current_dir);
        app->clipboard.paths.clear();
        app->clipboard.archive[0] = '\0';
    } else {
        snprintf(app->status_msg, sizeof(app->status_msg), "Extract failed: %s", err.c_str());
    }
    refresh_pane(&active, active.current_dir);
    refresh_pane(&other, other.current_dir);
}

// TC F5 inside an archive: the marked members, or the row under the cursor, are
// copied into the other pane's directory. A member never exists on disk, so the
// copy is an extraction of exactly those members, and each row's display path is
// mapped through the listing to the `raw` name the tool wrote. The archive's
// internal path is kept, so copying out of a folder inside the archive recreates
// that folder in the destination — the same rule Alt+F6 extraction follows, and
// what keeps members from different levels of one archive colliding.
// ---------------------------------------------------------------------------
// The operation plan: what F5/F6 asked for, decided once and then run
// ---------------------------------------------------------------------------

// Fill a plan from the active pane's targets: everything marked, or the row
// under the cursor when nothing is marked. Inside an archive the targets are
// members, so the plan carries the archive and the names the tool wants too,
// which are not always the names the pane shows.
static bool plan_from_pane(AppPanes* app, OpPlan& plan, bool is_move) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;
    plan = OpPlan();
    plan.is_move = is_move;

    if (in_archive(active)) {
        // Only copy-out exists here: a move would have to delete the member
        // afterwards, and no format we can write lets us do that.
        if (is_move) {
            snprintf(app->status_msg, sizeof(app->status_msg),
                     "Cannot move archive members: an archive cannot be written into (F5 copies them out)");
            return false;
        }
        if (in_archive(other)) {
            snprintf(app->status_msg, sizeof(app->status_msg), "The target pane is inside an archive");
            return false;
        }
        plan.archive = active.archive_path;
        plan.archive_prefix = active.archive_prefix;
        plan.src_dir = active.archive_path;
        plan.dst_dir = other.current_dir;
        plan.names = target_rel(active);
        for (const std::string& rel : plan.names)
            plan.raw_names.push_back(member_raw(active, rel.c_str()));
    } else if (in_remote(active)) {
        // Out of a site, one direction at a time. Between two sites there is no
        // path that does not come through this machine first, and that has not
        // been decided, so it is refused rather than quietly doing something else.
        if (in_remote(other)) {
            snprintf(app->status_msg, sizeof(app->status_msg),
                     "Both panes are on a site: F5 downloads into a local pane");
            return false;
        }
        if (in_archive(other)) {
            snprintf(app->status_msg, sizeof(app->status_msg), "The target pane is inside an archive");
            return false;
        }
        plan.remote = active.remote_root;
        plan.remote_prefix = active.remote_path;
        plan.src_dir = active.current_dir;
        plan.dst_dir = other.current_dir;
        plan.names = target_rel(active);
    } else if (in_remote(other)) {
        // Into a site: the local pane's entries go up. A move drops the local copy
        // once the server has its own.
        plan.src_dir = active.current_dir;
        plan.dst_dir = other.current_dir;
        plan.names = target_rel(active);
    } else {
        if (archive_readonly(app, other)) return false;
        plan.src_dir = active.current_dir;
        plan.dst_dir = other.current_dir;
        plan.names = target_rel(active);
    }
    if (plan.names.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to %s", is_move ? "move" : "copy");
        return false;
    }
    return true;
}

// Where a target lands: the destination directory plus the name it keeps, or
// the one the dialog typed. Archive members keep their path inside the archive,
// which is what the extraction tools write and what the user sees in the pane.
static std::string plan_target(const OpPlan& plan, size_t i) {
    std::string rel = plan.names[i];
    if (!plan.archive.empty() && !plan.archive_prefix.empty())
        rel = plan.archive_prefix + "/" + rel;
    if (!plan.name_override.empty()) rel = plan.name_override;
    return plan.dst_dir + "/" + rel;
}

static int plan_existing(const OpPlan& plan) {
    int n = 0;
    std::string droot, drpath;
    if (net_split_virtual(plan.dst_dir.c_str(), &droot, &drpath)) {
        // An upload overwrites just the same, so the same confirm has to happen
        // before it does. Only the server knows what is already there, and the
        // answer is one listing of the destination directory. It cannot be asked
        // per name: there is no single-entry lookup on a server, so net_stat()
        // lists the parent itself, and a per-name loop would pay for the same
        // listing once per name.
        const NetSite* site = net_site_for(droot);
        if (!site) return 0;
        std::vector<FileEntry> rows;
        std::string err;
        if (!net_list(*site, drpath, &rows, &err)) return 0;
        for (size_t i = 0; i < plan.names.size(); i++) {
            std::string base = plan.name_override.empty() ? plan.names[i] : plan.name_override;
            for (const FileEntry& fe : rows)
                if (!strcmp(fe.name, base.c_str())) { n++; break; }
        }
        return n;
    }
    for (size_t i = 0; i < plan.names.size(); i++)
        if (fs_exists(plan_target(plan, i).c_str())) n++;
    return n;
}

// Run one copy or move over the plan's names. The names, not the pane's marks,
// decide what gets touched: the dialog showed a list, and a click while the
// overwrite confirm is up must not change what the operation acts on.
static int run_names_prog(AppPanes* app, const OpPlan& plan, fs_progress_fn cb, void* user) {
    int ok = 0;
    for (size_t i = 0; i < plan.names.size(); i++) {
        std::string src = plan.src_dir + "/" + plan.names[i];
        std::string dst = plan_target(plan, i);
        bool r = plan.is_move ? fs_move_entry_progress(src.c_str(), dst.c_str(), cb, user)
                              : fs_copy_entry_progress(src.c_str(), dst.c_str(), cb, user);
        if (r) ok++;
        if (app->quit_request || app->prog_abort) break;
    }
    return ok;
}

// Show the result wherever it was typed to land: whichever pane is looking at
// the destination, or at a directory that holds it, gets refreshed.
static void refresh_for_destination(AppPanes* app, const std::string& dst) {
    Pane* panes[2] = { &app->left, &app->right };
    for (Pane* p : panes) {
        if (in_archive(*p)) continue;
        std::string cur = p->current_dir;
        if (dst == cur ||
            (dst.size() > cur.size() && dst.compare(0, cur.size(), cur) == 0 && dst[cur.size()] == '/'))
            refresh_pane(p, p->current_dir);
    }
}

// ---------------------------------------------------------------------------
// Background transfers. A copy or move that crosses a network is the one
// operation that can run for minutes, and the only one where a thread buys
// something: the panes stay usable while it goes, and the list under them says
// what the thread is doing. Everything else stays synchronous - a local copy is
// bounded by the disk, and two threads writing into one directory tree is a new
// problem bought for no gain.
//
// The rule that keeps this safe is ownership. The worker gets a copy of the site
// and a copy of the plan, reports into atomics, and never touches AppPanes, which
// the UI thread is drawing with at the same moment. libcurl is safe for the same
// reason it is safe in the panes: the transfer thread holds its own handle, and
// the handle cache is per thread (see remote.cpp).
// ---------------------------------------------------------------------------

// The worker's progress callback. Same contract as copy_progress_cb - the numbers
// up, false to stop - but it publishes instead of drawing. This thread has no
// business inside ImGui.
static bool bg_progress_cb(long long done, long long total, int fd, int ft, void* user) {
    BgTransfer* t = (BgTransfer*)user;
    t->done        = done;
    t->total       = total;
    t->files_done  = fd;
    t->files_total = ft;
    // Quitting asks every transfer to stop (app_panes_free), so the request
    // reaches the transport through the same flag the Abort button sets.
    return !t->abort.load();
}

// Seconds as a clock. An ETA of "3720s" is a number the user has to do
// arithmetic on; "1h02m" is not.
static void fmt_secs(double s, char* out, size_t n) {
    if (!(s > 0.0)) s = 0.0;            // also catches a NaN rate from a tiny transfer
    long t = (long)(s + 0.5);
    if (t < 60)        snprintf(out, n, "%lds", t);
    else if (t < 3600) snprintf(out, n, "%ldm%02lds", t / 60, t % 60);
    else               snprintf(out, n, "%ldh%02ldm", t / 3600, (t % 3600) / 60);
}

static const char* bg_state_name(int st) {
    switch (st) {
    case BgTransfer::BG_DONE:    return "done";
    case BgTransfer::BG_FAILED:  return "failed";
    case BgTransfer::BG_ABORTED: return "stopped";
    default:                     return "running";
    }
}

// The word or number drawn on the bar. A running row says so by moving, and what
// a bar cannot say by itself is how far along it is, so that is the number that
// goes on it. A row that has finished keeps its word instead: "done" is the news,
// "100%" is not. A running row that has no total yet - the listing has not come
// back, or the server would not say the size - has no percentage to show, so it
// falls back to the word.
static void bg_overlay(int st, long long done, long long total, char* out, size_t n) {
    if (st == BgTransfer::BG_RUNNING && total > 0) {
        int pct = (int)((double)done / (double)total * 100.0 + 0.5);
        if (pct > 100) pct = 100;      // a total that grew mid-transfer is not the row's problem
        snprintf(out, n, "%d%%", pct);
        return;
    }
    snprintf(out, n, "%s", bg_state_name(st));
}

// One line of numbers for the transfer list: how much has moved, how much is
// left, how fast, and when it will be over. Kept apart from the drawing so the
// arithmetic can be checked without a display, and so a transfer that has not
// reported a size yet says so instead of dividing by zero.
static void bg_line(const BgTransfer* t, char* out, size_t n) {
    long long done  = t->done.load();
    long long total = t->total.load();
    double now     = SDL_GetTicks() / 1000.0;
    double elapsed = now - t->t0;
    if (elapsed < 0.0) elapsed = 0.0;
    double rate = elapsed > 0.05 ? (double)done / elapsed : 0.0;

    char done_s[32], total_s[32], left_s[32], rate_s[32];
    human_size((off_t)done, done_s, sizeof(done_s));
    human_size((off_t)total, total_s, sizeof(total_s));
    human_size((off_t)(total > done ? total - done : 0), left_s, sizeof(left_s));
    human_size((off_t)rate, rate_s, sizeof(rate_s));

    // A finished row has no rate to speak of - the average over a transfer that
    // is over is not what anyone wants to read - so it reports what it moved and
    // how long that took.
    if (t->state.load(std::memory_order_acquire) != BgTransfer::BG_RUNNING) {
        snprintf(out, n, "%s in %.0fs", done_s, elapsed);
        return;
    }
    if (total <= 0) {
        // A tree whose sizes the server has not given yet. Saying "unknown" is
        // better than a bar that never moves.
        snprintf(out, n, "%s so far  %s/s  size not known yet", done_s, rate_s);
        return;
    }
    char eta[32] = "";
    if (rate > 1.0) fmt_secs((double)(total - done) / rate, eta, sizeof(eta));
    snprintf(out, n, "%s of %s   %s left   %s/s%s%s", done_s, total_s, left_s, rate_s,
             eta[0] ? "   eta " : "", eta);
}

// The worker's half of a copy or move that crosses the network: the same transfer
// the dialog used to drive on the UI thread, the same rule that a move across a
// network is a transfer and then a delete of the source, and the same order - the
// source is only removed once the copy is known good. What is different is that it
// may not touch AppPanes. The site arrives by value for the same reason: the user
// can edit or remove the saved site mid-transfer, and the transfer keeps the login
// it started with.
static void bg_run_remote(BgTransfer* t, const NetSite& site, const OpPlan& plan) {
    // Pure string work, no globals, so it is safe to do here rather than hand the
    // result over with the plan.
    std::string droot, drpath;
    net_split_virtual(plan.dst_dir.c_str(), &droot, &drpath);

    std::string err;
    int ok = 0;
    for (size_t i = 0; i < plan.names.size(); i++) {
        std::string base = plan.name_override.empty() ? plan.names[i] : plan.name_override;
        bool r;
        if (t->download) {
            std::string src = net_join_remote(plan.remote_prefix, plan.names[i]);
            std::string local = plan.dst_dir + "/" + base;
            r = net_download_tree(site, src, local.c_str(), bg_progress_cb, t, &err);
            if (r && plan.is_move) net_remove_tree(site, src, bg_progress_cb, t, &err);
        } else {
            std::string local = plan.src_dir + "/" + plan.names[i];
            std::string dst = drpath.empty() ? base : drpath + "/" + base;
            r = net_upload_tree(site, local.c_str(), dst, bg_progress_cb, t, &err);
            if (r && plan.is_move) fs_delete_entry_progress(local.c_str(), bg_progress_cb, t);
        }
        if (r) ok++;
        if (t->abort) break;
    }
    t->ok_names = ok;
    if (!ok)
        t->error = t->abort.load() ? "stopped before anything transferred"
                                   : (err.empty() ? std::string("transfer failed") : err);
    // The store is what publishes ok_names and error: a frame that reads the
    // state with acquire sees everything written before it.
    int st = t->abort.load() ? BgTransfer::BG_ABORTED
                             : (ok ? BgTransfer::BG_DONE : BgTransfer::BG_FAILED);
    t->state.store(st, std::memory_order_release);
}

// Join and drop the oldest finished row, so a long session does not fill the
// strip with history. A running transfer is never taken out from under the thread
// reporting into it, so if every row is running this gives up and the new
// transfer simply waits for a slot.
static void bg_retire_oldest_finished(AppPanes* app) {
    for (size_t i = 0; i < app->transfers.size(); i++) {
        BgTransfer* old = app->transfers[i].get();
        if (old->state.load(std::memory_order_acquire) == BgTransfer::BG_RUNNING) continue;
        if (old->worker.joinable()) old->worker.join();
        app->transfers.erase(app->transfers.begin() + (long)i);
        return;
    }
}

// Hand the plan to a thread. The plan arrives by value because run_op_plan()
// clears app->op_plan the moment this returns, and the sources have to outlive
// that. The row is returned so the caller can watch it: staying in the progress
// dialog is a choice about who is watching, not about where the copy runs.
static BgTransfer* bg_start_remote_plan(AppPanes* app, const OpPlan& plan) {
    std::string droot, drpath;
    bool dst_remote = net_split_virtual(plan.dst_dir.c_str(), &droot, &drpath);
    const NetSite* dsite = dst_remote ? net_site_for(droot) : nullptr;
    const NetSite* ssite = plan.remote.empty() ? nullptr : net_site_for(plan.remote);
    if ((dst_remote && !dsite) || (!plan.remote.empty() && !ssite)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "No login for %s: Ctrl+F edits the saved sites, Ctrl+N connects again",
                 dst_remote ? droot.c_str() : plan.remote.c_str());
        return nullptr;
    }
    // plan_from_pane() refuses site to site, so exactly one side is remote.
    const bool download = ssite != nullptr;
    const NetSite& site = download ? *ssite : *dsite;

    while (app->transfers.size() >= 12) {
        size_t before = app->transfers.size();
        bg_retire_oldest_finished(app);
        if (app->transfers.size() == before) break;
    }

    std::unique_ptr<BgTransfer> up = std::make_unique<BgTransfer>();
    BgTransfer* t = up.get();
    t->state  = BgTransfer::BG_RUNNING;
    t->abort  = false;
    t->settled = false;
    t->dismiss = false;
    t->done   = 0;
    t->total  = 0;
    t->files_done = 0;
    t->files_total = 0;
    t->is_move  = plan.is_move;
    t->download = download;
    t->t0       = SDL_GetTicks() / 1000.0;
    t->src      = plan.remote.empty()
                      ? plan.src_dir
                      : net_virtual_path(plan.remote, plan.remote_prefix);
    t->dst      = plan.dst_dir;
    // Everything above is fixed before the thread starts, so the worker never
    // sees a half-built record.
    t->worker = std::thread(bg_run_remote, t, site, plan);
    app->transfers.push_back(std::move(up));
    return t;
}

// Between frames, before anything is drawn. A transfer that has finished gets its
// destination refreshed and its result in the status bar exactly once; a row the
// user dismissed is joined and removed. The join happens here rather than in the
// widget so that no button press can wait on a thread inside an ImGui frame - the
// thread is already finished by then, but the ordering is the part that matters.
static void bg_reap(AppPanes* app) {
    for (size_t i = 0; i < app->transfers.size();) {
        BgTransfer* t = app->transfers[i].get();
        bool running = t->state.load(std::memory_order_acquire) == BgTransfer::BG_RUNNING;
        if (!running && !t->settled.exchange(true)) {
            if (t->ok_names)
                snprintf(app->status_msg, sizeof(app->status_msg),
                         "%s %d item%s to %s%s", t->is_move ? "Moved" : "Copied", t->ok_names,
                         t->ok_names == 1 ? "" : "s", t->dst.c_str(),
                         t->abort.load() ? " (aborted)" : "");
            else
                snprintf(app->status_msg, sizeof(app->status_msg), "%s failed: %s",
                         t->is_move ? "Move" : "Copy", t->error.c_str());
            refresh_for_destination(app, t->dst);
        }
        if (!running && t->dismiss.load()) {
            if (t->worker.joinable()) t->worker.join();
            app->transfers.erase(app->transfers.begin() + (long)i);
            continue;
        }
        i++;
    }
}

// Ask every running transfer to stop and wait for it to finish the file it is on.
// This is what Quit does, and it is the same promise the Abort button makes: the
// stop lands between files, never in the middle of one, because a half-written
// file is worse than no file.
static void bg_stop_all(AppPanes* app) {
    for (std::unique_ptr<BgTransfer>& t : app->transfers) t->abort = true;
    for (std::unique_ptr<BgTransfer>& t : app->transfers) {
        if (t->worker.joinable()) t->worker.join();
    }
    app->transfers.clear();
}

// How much room the transfer strip needs, so the panes can be given what is left
// of the window before the strip is drawn. One row per transfer plus a header, at
// most seven rows before the list scrolls.
static float bg_strip_height(const AppPanes* app, float frame) {
    if (app->transfers.empty()) return 0.0f;
    int rows = 1 + (int)app->transfers.size();
    if (rows > 7) rows = 7;
    return 6.0f + (float)rows * frame;
}

// The transfer list, between the panes and the command line. It claims no space
// while it is empty, so a session that never touches the network never sees it.
static void render_transfers(AppPanes* app, float h) {
    if (app->transfers.empty()) return;
    ImGui::Spacing();
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (app->transfers.size() > 6) flags |= ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("bg_transfers", 5, flags, ImVec2(0, h - 6.0f))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 40.0f);   // get / put
    ImGui::TableSetupColumn("Transfer", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 150.0f);  // the bar
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 330.0f);  // the numbers
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 80.0f);   // the button

    // Header: how many, and one place to clear the finished rows, which
    // otherwise sit there until the session ends.
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%d transfer%s", (int)app->transfers.size(),
                        app->transfers.size() == 1 ? "" : "s");
    ImGui::TableNextColumn();
    bool any_finished = false;
    for (const std::unique_ptr<BgTransfer>& t : app->transfers)
        if (t->state.load(std::memory_order_acquire) != BgTransfer::BG_RUNNING) any_finished = true;
    if (any_finished && ImGui::SmallButton("clear finished"))
        for (std::unique_ptr<BgTransfer>& t : app->transfers)
            if (t->state.load(std::memory_order_acquire) != BgTransfer::BG_RUNNING) t->dismiss = true;
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();

    for (std::unique_ptr<BgTransfer>& up : app->transfers) {
        BgTransfer* t = up.get();
        int st = t->state.load(std::memory_order_acquire);
        bool running = st == BgTransfer::BG_RUNNING;
        // The rows all carry the same button labels, so the pointer is the id.
        ImGui::PushID(t);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", t->download ? "get" : "put");

        ImGui::TableNextColumn();
        // The name that is moving, not the whole path: the path is what the row
        // is too narrow for, and the tooltip carries it.
        std::string base = t->src;
        size_t slash = base.find_last_of('/');
        if (slash != std::string::npos && slash + 1 < base.size()) base = base.substr(slash + 1);
        std::string what = base + (t->is_move ? " (move)" : "");
        ImGui::TextUnformatted(elide_middle(what.c_str(), ImGui::GetContentRegionAvail().x).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n-> %s\n%s\nfiles: %d of %d", t->src.c_str(), t->dst.c_str(),
                              bg_state_name(st), t->files_done.load(), t->files_total.load());

        ImGui::TableNextColumn();
        long long done = t->done.load(), total = t->total.load();
        float frac = total > 0 ? (float)((double)done / (double)total)
                               : (running ? 0.0f : 1.0f);
        char overlay[32];
        bg_overlay(st, done, total, overlay, sizeof(overlay));
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);

        ImGui::TableNextColumn();
        char line[256];
        bg_line(t, line, sizeof(line));
        // A row that ended badly is read for the reason, not for its averages.
        if (!running && !t->error.empty())
            text_wrap_colored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", t->error.c_str());
        else
            ImGui::TextUnformatted(line);

        ImGui::TableNextColumn();
        // Abort is a request, not a stop: the transport polls the progress
        // callback, which returns false while this is set, so the transfer
        // unwinds after the file it is on. "clear" only appears once it has.
        if (running) {
            if (ImGui::SmallButton(t->abort.load() ? "stopping" : "Abort")) t->abort = true;
        } else {
            if (ImGui::SmallButton("clear")) t->dismiss = true;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// The one runner for copy, move and copying out of an archive. The dialog, the
// overwrite confirm and the progress dialog all describe this same plan, so
// they cannot end up talking about different operations.
static void run_op_plan(AppPanes* app) {
    OpPlan plan = app->op_plan;
    app->op_plan = OpPlan();
    // Taken here and cleared here, so the choice cannot leak into the next
    // operation whatever kind of plan this one turned out to be.
    bool run_bg = app->op_run_bg;
    app->op_run_bg = false;
    Pane& active = app->left_focus ? app->left : app->right;

    // Remember the slot being vacated so the cursor can land on the entry that
    // moves up into it (or the one above if the list got shorter).
    int slot = active.selected;
    if (plan.is_move && marked_count(active) > 0)
        for (int i = 0; i < active.entry_count; i++)
            if (active.marked[i]) { slot = i; break; }

    if (!plan.archive.empty()) {
        arch_prog_begin(app, plan.archive.c_str(), plan.dst_dir.c_str());
        std::string err;
        bool ok = arch_extract_members(plan.archive.c_str(), plan.dst_dir.c_str(), plan.raw_names,
                                       copy_progress_cb, app, &err);
        app->prog_active = false;
        if (ok)
            snprintf(app->status_msg, sizeof(app->status_msg), "Copied %d member%s into %s%s%s",
                     (int)plan.names.size(), plan.names.size() == 1 ? "" : "s", plan.dst_dir.c_str(),
                     plan.archive_prefix.empty() ? "" : " (archive paths preserved)",
                     app->prog_abort ? " (aborted)" : "");
        else if (app->prog_abort)
            snprintf(app->status_msg, sizeof(app->status_msg), "Extract stopped before anything copied");
        else
            snprintf(app->status_msg, sizeof(app->status_msg), "Copy failed: %s", err.c_str());
        refresh_for_destination(app, plan.dst_dir);
        return;
    }

    if (!plan.remote.empty() || net_is_remote(plan.dst_dir.c_str())) {
        // The one operation worth a thread, and worth it either way: a transfer
        // over a network can run for minutes, and the panes do not have to be
        // frozen while it does. Running it on the thread even when the user chose
        // to wait is what makes "Run in background" a decision that can still be
        // taken halfway through, instead of one that has to be taken in advance.
        BgTransfer* t = bg_start_remote_plan(app, plan);
        if (!t) return;
        if (run_bg)
            snprintf(app->status_msg, sizeof(app->status_msg),
                     "%s %s in the background: %s -> %s",
                     plan.is_move ? "Moving" : "Copying",
                     plan.names.size() == 1 ? "item" : "items", t->src.c_str(), t->dst.c_str());
        else
            watch_remote_transfer(app, t);
        return;
    }

    prog_begin(app, plan.src_dir.c_str(), plan.dst_dir.c_str(), plan.is_move);
    int ok = run_names_prog(app, plan, copy_progress_cb, app);
    app->prog_active = false;
    // An abort is not a failure, and saying so matters: the entries that made it
    // across are really there, and the rest were never attempted.
    if (ok)
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "%s %d item%s to %s%s", plan.is_move ? "Moved" : "Copied",
                 ok, ok == 1 ? "" : "s", plan.dst_dir.c_str(),
                 app->prog_abort ? " (aborted)" : "");
    else if (app->prog_abort)
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "%s stopped before anything transferred", plan.is_move ? "Move" : "Copy");
    else
        snprintf(app->status_msg, sizeof(app->status_msg),
                 plan.is_move ? "Move failed: %s" : "Copy failed: %s", strerror(errno));

    // Afterwards, not before: a pane that is looking at the destination has to
    // show what landed there, and the pane the entries left has to lose them.
    refresh_for_destination(app, plan.dst_dir);
    if (plan.is_move) {
        refresh_pane(&active, active.current_dir);
        if (ok && active.entry_count > 0) {
            if (slot > active.entry_count - 1) slot = active.entry_count - 1;
            if (slot < 0) slot = 0;
            active.selected = slot;
            active.anchor = slot;
            active.scroll_request = true;
        }
    }
}

// F5 and F6 open a dialog instead of acting: it names the sources, shows where
// they are going, and lets the destination - or a new name for a single entry -
// be edited before anything is written.
static void open_op_dialog(AppPanes* app, const OpPlan& plan) {
    app->op_plan = plan;
    app->op = plan.is_move ? OP_MOVE : OP_COPY;
    app->op_error[0] = '\0';
    app->op_run_bg = false;
    app->op_focus_request = true;
    // One entry: the field holds the whole target, so changing just the name is
    // one edit. Several, or members coming out of an archive: it holds the
    // directory, which keeps every entry's own name.
    if (plan.names.size() == 1 && plan.archive.empty())
        snprintf(app->op_name, sizeof(app->op_name), "%s/%s", plan.dst_dir.c_str(), plan.names[0].c_str());
    else
        snprintf(app->op_name, sizeof(app->op_name), "%s", plan.dst_dir.c_str());
}

// Open the dialog for a name-taking operation (F7 folder, Shift+F4 file, F2
// rename). The field is prefilled so the common case is Enter rather than
// typing: rename starts from the current name, mkdir and new file start empty.
static void open_name_dialog(AppPanes* app, AppOp op, const char* prefill) {
    Pane& active = app->left_focus ? app->left : app->right;
    app->op = op;
    snprintf(app->op_name, sizeof(app->op_name), "%s", prefill ? prefill : "");
    // The folder the name lands in, and for a rename the entry being renamed, are
    // fixed here rather than read back at commit time: clicking the other pane
    // while the dialog is up must not move the operation underneath it.
    app->op_plan = OpPlan();
    app->op_plan.src_dir = active.current_dir;
    if (op == OP_RENAME && active.selected >= 0 && active.selected < active.entry_count)
        app->op_plan.names.push_back(active.entries[active.selected]->name);
    app->op_error[0] = '\0';
    app->op_focus_request = true;
}

static void do_copy(AppPanes* app) {
    OpPlan plan;
    if (!plan_from_pane(app, plan, false)) return;
    open_op_dialog(app, plan);
}

static void do_move(AppPanes* app) {
    OpPlan plan;
    if (!plan_from_pane(app, plan, true)) return;
    open_op_dialog(app, plan);
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
    if (remote_blocked(app, pane, "The file clipboard")) return;
    if (in_archive(pane)) {
        // Ctrl+C inside an archive puts members on the clipboard; pasting into a
        // real directory extracts them. Cutting is refused because a member
        // cannot be removed from an archive, and a clipboard that quietly
        // dropped the delete half of a cut would be worse than one that says no.
        if (cut) {
            snprintf(app->status_msg, sizeof(app->status_msg),
                     "Cannot cut archive members: an archive cannot be written into");
            return;
        }
        StringList members = target_rel(pane);
        if (members.empty()) {
            snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to copy");
            return;
        }
        // The clipboard carries the member's full path inside the archive, not
        // the level-relative display path: a paste happens somewhere else and
        // has to name the member unambiguously.
        if (pane.archive_prefix[0])
            for (std::string& m : members)
                m = std::string(pane.archive_prefix) + "/" + m;
        app->clipboard.paths = members;
        app->clipboard.cut = false;
        snprintf(app->clipboard.archive, sizeof(app->clipboard.archive), "%s", pane.archive_path);
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "%d member%s on the clipboard (copy)", (int)members.size(),
                 members.size() == 1 ? "" : "s");
        return;
    }
    StringList paths = target_paths(pane);
    if (paths.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Nothing to %s",
                 cut ? "cut" : "copy");
        return;
    }
    app->clipboard.paths = paths;
    app->clipboard.cut = cut;
    app->clipboard.archive[0] = '\0';
    snprintf(app->status_msg, sizeof(app->status_msg),
             "%d entr%s on the clipboard (%s)", (int)paths.size(),
             paths.size() == 1 ? "y" : "ies", cut ? "cut" : "copy");
}

// TC Ctrl+P: put the pane's path in the command line. Text that is already
// there is kept and the path appended, so "rm " + path works; the command
// buffer is shorter than a path can be, so a deep directory truncates.
static void cmd_put_path(AppPanes* app, Pane& pane) {
    size_t used = strlen(app->cmd);
    if (used && app->cmd[used - 1] != ' ') {
        if (used + 1 < sizeof(app->cmd)) app->cmd[used++] = ' ';
    }
    app->cmd[used] = '\0';
    // Inside an archive the pane's path is virtual and no shell would accept it,
    // so the archive file itself is what is useful to put on the line.
    snprintf(app->cmd + used, sizeof(app->cmd) - used, "%s",
             in_archive(pane) ? pane.archive_path : pane.current_dir);
    snprintf(app->status_msg, sizeof(app->status_msg), "Path into the command line");
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
    if (app->clipboard.archive[0]) { do_paste_archive_now(app); return; }
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other  = app->left_focus ? app->right : app->left;
    if (app->clipboard.paths.empty()) return;

    prog_begin(app, active.current_dir, active.current_dir, app->clipboard.cut);
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
        if (app->quit_request || app->prog_abort) {
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
    if (archive_readonly(app, active)) return;
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
    case CONF_OVERWRITE:
    case CONF_OVERWRITE_MOVE:
    case CONF_OVERWRITE_EXTRACT: run_op_plan(app); break;
    case CONF_OVERWRITE_PASTE: do_paste_now(app); break;
    default: break;
    }
    app->confirm = CONF_NONE;
    app->confirm_text[0] = '\0';
}

// Deferred actions from menu/context-menu/dialog widgets: these fire inside
// the ImGui frame, but the operations below block and pump their own progress
// frames, so they must run between frames.
void app_panes_run_pending(AppPanes* app) {
    if (app->pending_confirm_yes) {
        app->pending_confirm_yes = false;
        confirm_yes(app);
    }
    if (!app->pending_op) return;
    int op = app->pending_op;
    app->pending_op = DEFER_NONE;
    switch (op) {
    case DEFER_COPY:          do_copy(app); break;   // opens the dialog
    case DEFER_MOVE:          do_move(app); break;   // opens the dialog
    case DEFER_OP_COMMIT:     app_panes_commit_op(app); break;
    case DEFER_OP_RUN:        run_op_plan(app); break;
    case DEFER_PASTE:         do_paste(app); break;
    case DEFER_EXTRACT:       do_extract(app, true); break;
    case DEFER_PACK:          pack_commit(app); break;
    case DEFER_COPY_IN_PLACE: do_copy_in_place(app); break;
    case DEFER_SYMLINK_OTHER: do_symlink_other(app); break;
    case DEFER_FILL_SIZES:    fill_dir_sizes(app); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Viewer + entry activation
// ---------------------------------------------------------------------------

static char viewer_buf[256 * 1024];
static float viewer_scroll = 0.0f;
static bool  viewer_dirty = false;

// The viewer can be showing a member unpacked into a temporary directory, so
// closing it has to clean that up. One place so both close paths agree.
static void close_viewer(AppPanes* app) {
    app->viewer_open = false;
    if (app->viewer_tmp_dir[0]) {
        char cmd[4200];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", app->viewer_tmp_dir);
        int rc = system(cmd);
        (void)rc;
        app->viewer_tmp_dir[0] = '\0';
    }
}

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
    if (in_remote(pane)) {
        // Branch view is one recursive walk. Over FTP that is a listing per
        // directory, and the pane already shows the same rows one level at a time.
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Branch view needs a local directory (Backspace leaves the site)");
        return;
    }
    if (in_archive(pane)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Branch view needs a directory (Backspace leaves the archive)");
        return;
    }
    if (pane.list_mode == LIST_BRANCH) {
        refresh_pane(&pane, pane.current_dir);
        snprintf(app->status_msg, sizeof(app->status_msg), "Branch view off");
        return;
    }
    branch_fill(app, pane, marked_only);
}

// The tool-facing name of a member the pane is showing. A row's rel is relative
// to the level being listed, which is what the virtual path needs, while the
// tool wants the member's full path inside the archive (and tar can store that
// with a "./" prefix, so the listing is re-read to map one onto the other).
// Ops are rare enough that the extra listing is cheaper than a second parallel
// vector on every row.
static std::string member_raw(const Pane& pane, const char* rel) {
    std::string full = rel ? rel : "";
    if (pane.archive_prefix[0]) full = std::string(pane.archive_prefix) + "/" + full;
    std::vector<ArchEntry> members;
    std::string err;
    if (arch_list(pane.archive_path, &members, &err)) {
        const ArchEntry* e = arch_find(members, full.c_str());
        if (e) return e->raw;
    }
    return full;
}

// Fetch one remote file into a temporary directory the viewer can read. The
// directory is remembered on the app so closing the viewer removes it, exactly
// as an extracted archive member is.
static bool remote_fetch_temp(AppPanes* app, const NetSite& site, const std::string& rpath,
                              char* out_path, size_t npath, char* out_dir, size_t ndir) {
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char tmpl[4096];
    snprintf(tmpl, sizeof(tmpl), "%s/mc-clone-ftp-XXXXXX", tmp);
    if (!mkdtemp(tmpl)) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Cannot make a temporary directory: %s",
                 strerror(errno));
        return false;
    }
    snprintf(out_dir, ndir, "%s", tmpl);
    const char* base = strrchr(rpath.c_str(), '/');
    snprintf(out_path, npath, "%s/%s", tmpl, base ? base + 1 : rpath.c_str());
    std::string err;
    if (net_download(site, rpath, out_path, copy_progress_cb, app, &err)) return true;
    // A failed transfer leaves no file behind, so only the directory is left to
    // clean up - and the viewer must not be handed a directory it cannot read.
    rmdir(tmpl);
    out_dir[0] = '\0';
    snprintf(app->status_msg, sizeof(app->status_msg), "Cannot fetch %s: %s",
             rpath.c_str(), err.c_str());
    return false;
}

// Open a row in the viewer. Inside an archive the member is unpacked to a
// temporary file first, and on a remote site the file is fetched to one; the
// viewer takes that temporary directory away when it closes. Everywhere else the
// row's path is already a real file.
static void view_entry(AppPanes* app, Pane& pane, int idx) {
    if (idx < 0 || idx >= pane.entry_count) return;
    const char* rel = entry_rel(pane, idx);
    if (in_archive(pane)) {
        char tmp[4300], tmpdir[4096];
        std::string err;
        if (arch_extract_temp(pane.archive_path, member_raw(pane, rel).c_str(),
                              tmp, sizeof(tmp), tmpdir, sizeof(tmpdir), &err)) {
            snprintf(app->viewer_tmp_dir, sizeof(app->viewer_tmp_dir), "%s", tmpdir);
            open_viewer(app, tmp);
        } else {
            snprintf(app->status_msg, sizeof(app->status_msg), "Cannot open %s: %s",
                     pane.entries[idx]->name, err.c_str());
        }
        return;
    }
    if (in_remote(pane)) {
        const NetSite* site = remote_site_of(app, pane);
        if (!site) return;
        char tmp[4300], tmpdir[4096];
        if (remote_fetch_temp(app, *site, remote_row_path(pane, rel),
                              tmp, sizeof(tmp), tmpdir, sizeof(tmpdir))) {
            snprintf(app->viewer_tmp_dir, sizeof(app->viewer_tmp_dir), "%s", tmpdir);
            open_viewer(app, tmp);
        }
        return;
    }
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", pane.current_dir, rel);
    open_viewer(app, full);
}

static void activate_entry(AppPanes* app, Pane& pane, int idx) {
    if (idx < 0 || idx >= pane.entry_count) return;
    FileEntry* e = pane.entries[idx];
    const char* rel = entry_rel(pane, idx);
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", pane.current_dir, rel);
    if (strcmp(e->name, "..") == 0) {
        if (in_remote(pane)) {
            // Up inside a site is one path component; up out of the site itself
            // leaves it, back to wherever the pane was before the connect. The
            // question is "is this the site root", not "is the path non-empty":
            // net_path_parent() answers "" for the root, so a non-empty path one
            // level down would otherwise step to the root and then sit there.
            if (!net_path_is_root(pane.remote_path)) {
                std::string up = net_path_parent(pane.remote_path);
                navigate_to(app, &pane, net_virtual_path(pane.remote_root, up).c_str());
            } else {
                navigate_to(app, &pane, pane.remote_came_from[0] ? pane.remote_came_from : "/");
            }
            return;
        }
        char parentbuf[4096];
        snprintf(parentbuf, sizeof(parentbuf), "%s", pane.current_dir);
        char* parent = dirname(parentbuf);
        navigate_to(app, &pane, parent);
    } else if (is_directory(e->name, e->mode)) {
        char path[4100];
        snprintf(path, sizeof(path), "%s/%s", pane.current_dir, rel);
        navigate_to(app, &pane, path);
    } else if (!in_remote(pane) && arch_is_archive(full)) {
        // TC opens an archive as if it were a directory. The path stays virtual
        // from here on: everything below it is a member, not a file. A remote row
        // only looks like a local path, so the extension is not enough to open it.
        navigate_to(app, &pane, full);
    } else if (pane.archive_path[0]) {
        // A member of the archive being browsed: unpack just this one rather
        // than the whole archive.
        view_entry(app, pane, idx);
    } else if (is_executable(e->name, e->mode) && !in_remote(pane)) {
        // A server's mode bits can say "executable" about a file this machine
        // cannot run, and would not be allowed to.
        char full[4300];
        snprintf(full, sizeof(full), "\"%s/%s\" 2>&1", pane.current_dir, rel);
        run_shell_and_show(app, full);
    } else {
        view_entry(app, pane, idx);
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

// The cursor-bound actions, shared by the key handler and the File menu. A menu
// item that reimplements its shortcut is how a menu ends up doing something the
// key does not, so both call these.
static void rename_cursor(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (branch_blocked(app, active)) return;
    if (active.selected < active.entry_count && strcmp(active.entries[active.selected]->name, "..") != 0)
        open_name_dialog(app, OP_RENAME, active.entries[active.selected]->name);
}

static void edit_cursor(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (active.selected >= active.entry_count) return;
    FileEntry* e = active.entries[active.selected];
    if (remote_blocked(app, active, "The editor")) return;
    if (in_archive(active)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Editing an archive member needs a real file (Alt+F9 extracts it)");
        return;
    }
    if (is_directory(e->name, e->mode)) return;
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", active.current_dir, entry_rel(active, active.selected));
    spawn_terminal_editor(app, full);
}

static void open_cursor_default(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (active.selected >= active.entry_count) return;
    if (remote_blocked(app, active, "Opening with the default program")) return;
    if (in_archive(active)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "An archive member has to be extracted first (Alt+F9)");
        return;
    }
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", active.current_dir, entry_rel(active, active.selected));
    open_with_default(app, full);
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
    if (in_remote(pane)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "The tree needs a local directory (Backspace leaves the site)");
        return;
    }
    if (in_archive(pane)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "The tree needs a real directory (Backspace leaves the archive)");
        return;
    }
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
    // Any click inside the tree focuses this pane, not only a click that happens
    // to land on a row. IsItemHovered() here would look at whatever widget came
    // before the child window, so it never fired for the pane's own area.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0))
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
    if (ImGui::MenuItem("View")) view_entry(app, pane, pane.selected);
    if (ImGui::MenuItem("Rename")) open_name_dialog(app, OP_RENAME, e->name);
    bool marked = pane.selected < (int)pane.marked.size() && pane.marked[pane.selected];
    if (ImGui::MenuItem(marked ? "Unmark" : "Mark")) toggle_mark(pane, pane.selected);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy to other pane")) app->pending_op = DEFER_COPY;
    if (ImGui::MenuItem("Move to other pane")) app->pending_op = DEFER_MOVE;
    if (ImGui::MenuItem("Paste here", "Ctrl+V")) app->pending_op = DEFER_PASTE;
    ImGui::Separator();
    if (ImGui::MenuItem("Delete")) request_delete(app, false);
    if (ImGui::MenuItem(app->trash_mode ? "Deletes go to trash" : "Deletes are always direct"))
        app->trash_mode = app->trash_mode ? 0 : 1;
}

// Both panes draw their strip into the same window, so the strip has to carry
// the pane in its ID: a tab labelled "work" and the add-tab "+" would otherwise
// be the same item twice, and ImGui only notices while the mouse is over one of
// them, which is how it stayed hidden until someone hovered the +.
static void render_tab_bar(AppPanes* app, Pane& pane, const char* id) {
    ImGui::PushID(id);
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
    ImGui::PopID();
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

// ---------------------------------------------------------------------------
// Quick view text
//
// Both quick views - the F3 window over a pane and the Ctrl+Q panel - show the
// beginning of the file under the cursor, and until now both could only read a
// file that exists on this disk: handed a site's path or an archive member's,
// fopen() fails and the preview says "unreadable" about a text file the pane is
// looking straight at. A local file is still read in the frame, because it costs
// microseconds. Anything else has to be fetched, and a fetch over a network is
// slower than the frame that asked for it, so it runs on a thread and the answer
// lands in a later frame. The cursor moves with the arrow keys, so an answer is
// usually late: a fetch carries the path it was asked for, and is used for that
// path and no other.

// Enough to read a screenful and then some, and small enough that previewing a
// 4 GB log costs the same as previewing a 4 KB one.
static const size_t k_qv_max_bytes = 64 * 1024;
// Previews kept, oldest first. Arrowing back and forth through one directory is
// the common movement, and a site should be asked once per name, not once per
// keypress.
static const size_t k_qv_cache_max = 32;
// Fetches allowed to run at once, which is one per quick view the app can show
// at a time: the F3 window over each pane and the Ctrl+Q panel. A fourth view
// would have to queue, and a queued view says "reading" forever, so the cap is
// not a queue depth. The fetches that stay alive after the cursor moved are the
// ones a frame is still waiting on; they are left running because joining one
// here would block the frame, which is the whole thing being avoided.
static const size_t k_qv_inflight_max = 3;

enum QvKind { QV_LOCAL, QV_ARCHIVE, QV_REMOTE };

// What has to be read, copied out of the pane before the thread starts: the pane
// navigates and a saved site gets edited while the thread is still running, so
// the thread reads none of the app.
struct QvJob {
    QvKind kind = QV_LOCAL;
    std::string key;      // the path the pane shows: what the answer is filed under
    std::string path;     // a local file, or the archive to unpack from
    std::string member;   // the member inside that archive, as the tool names it
    NetSite site;         // a copy of the site, password and all
    std::string rpath;    // the file on the server
};

// The answers a preview can only give in words.
static const char* k_qv_reading = "(reading ...)";
static const char* k_qv_binary  = "(binary: no text to show)";

// The path a pane shows for a row: a real path, or the virtual one for a row on
// a site or inside an archive.
static std::string pane_row_path(const Pane& pane, int idx) {
    char full[4100];
    snprintf(full, sizeof(full), "%s/%s", pane.current_dir, entry_rel(pane, idx));
    return full;
}

// Text or nothing: a NUL in what arrived is the tell. A CR that starts a newline
// is dropped and a lone CR becomes a newline, because ImGui advances over a
// carriage return without breaking the line, and a text file that came off a
// server is likely to have them.
static bool qv_to_text(std::string* body) {
    if (body->find('\0') != std::string::npos) return false;
    for (size_t i = body->find('\r'); i != std::string::npos; i = body->find('\r', i)) {
        if (i + 1 < body->size() && (*body)[i + 1] == '\n') body->erase(i, 1);
        else (*body)[i] = '\n';
    }
    return true;
}

static std::string* qv_cache_find(AppPanes* app, const std::string& key) {
    for (std::pair<std::string, std::string>& e : app->qv_cache)
        if (e.first == key) return &e.second;
    return nullptr;
}

static void qv_cache_put(AppPanes* app, const std::string& key, const std::string& text) {
    if (std::string* hit = qv_cache_find(app, key)) { *hit = text; return; }
    app->qv_cache.emplace_back(key, text);
    while (app->qv_cache.size() > k_qv_cache_max)
        app->qv_cache.erase(app->qv_cache.begin());
}

// The beginning of a file that is already on this disk.
static void qv_read_file(const std::string& path, std::string* body, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { *err = "cannot read " + path; return; }
    char buf[8192];
    size_t n;
    while (body->size() < k_qv_max_bytes && (n = fread(buf, 1, sizeof buf, f)) > 0)
        body->append(buf, n);
    fclose(f);
}

// Asked between chunks of a fetch: the cursor moved on, so stop pulling bytes
// that nobody is going to look at.
static bool qv_still_wanted_cb(long long, long long, int, int, void* user) {
    return !((QvFetch*)user)->cancel.load();
}

// The worker. It writes text and err and only then raises done, so a frame that
// sees done can read both. Nothing here touches the app, the panes, or ImGui.
static void qv_run_fetch(QvFetch* f, QvJob job) {
    std::string body, err, file = job.path;
    char tmpdir[4096];
    tmpdir[0] = '\0';

    if (job.kind == QV_REMOTE) {
        net_download_prefix(job.site, job.rpath, &body, k_qv_max_bytes,
                            qv_still_wanted_cb, f, &err);
    } else {
        if (job.kind == QV_ARCHIVE) {
            char tmp[4300];
            if (arch_extract_temp(job.path.c_str(), job.member.c_str(),
                                  tmp, sizeof tmp, tmpdir, sizeof tmpdir, &err))
                file = tmp;
        }
        if (err.empty()) qv_read_file(file, &body, &err);
    }
    if (tmpdir[0]) {
        char cmd[4200];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
        int rc = system(cmd);
        (void)rc;
    }

    if (err.empty() && !qv_to_text(&body)) body = k_qv_binary;
    f->text = err.empty() ? body : "";
    f->err = err;
    f->done = true;
}

static void qv_start(AppPanes* app, const QvJob& job) {
    std::unique_ptr<QvFetch> up = std::make_unique<QvFetch>();
    QvFetch* f = up.get();
    f->key = job.key;
    f->wanted = true;   // the frame that is drawing right now asked for it
    app->qv_fetches.push_back(std::move(up));
    f->worker = std::thread(qv_run_fetch, f, job);
}

// Between frames. A fetch that has answered is filed under the path it was asked
// for and dropped; the join is only ever of a thread that has finished, so this
// cannot stall a frame, and one still running is left alone until it answers.
// A fetch that failed is not cached, so the next look at that name asks again.
//
// A fetch that no quick view asked for in the frame that just drew is stale: the
// cursor moved on, its window closed, or its pane navigated away. Those are told
// to stop pulling bytes nobody will read. That question is asked here rather than
// where a new fetch starts, because two quick views can be open at once - F3 on
// each pane, or F3 and the Ctrl+Q panel - and a rule that cancelled everything
// else in order to start one view's fetch cancelled the other view's fetch, which
// restarted next frame and cancelled the first again: neither preview ever
// arrived, and the server took a connect and an abort per frame.
static void qv_reap(AppPanes* app) {
    for (std::unique_ptr<QvFetch>& f : app->qv_fetches) {
        if (!f->wanted) f->cancel = true;
        f->wanted = false;   // this frame's previews mark it again
    }
    for (size_t i = 0; i < app->qv_fetches.size();) {
        QvFetch* f = app->qv_fetches[i].get();
        if (!f->done.load()) { i++; continue; }
        if (f->err.empty()) qv_cache_put(app, f->key, f->text);
        f->worker.join();
        app->qv_fetches.erase(app->qv_fetches.begin() + i);
    }
}

// On the way out: every fetch stops and is joined before the transport is torn
// down under it.
static void qv_stop_all(AppPanes* app) {
    for (std::unique_ptr<QvFetch>& f : app->qv_fetches) f->cancel = true;
    for (std::unique_ptr<QvFetch>& f : app->qv_fetches)
        if (f->worker.joinable()) f->worker.join();
    app->qv_fetches.clear();
}

// What reading this row would take. False for a directory and for the rows that
// are not files, which have no text to show.
static bool qv_job_for(AppPanes* app, const Pane& pane, int idx, QvJob* job) {
    if (idx < 0 || idx >= pane.entry_count) return false;
    FileEntry* e = pane.entries[idx];
    if (is_directory(e->name, e->mode)) return false;
    if (in_remote(pane) && !remote_site_of(app, pane)) return false;

    job->key = pane_row_path(pane, idx);
    if (in_remote(pane)) {
        job->kind  = QV_REMOTE;
        job->site  = *remote_site_of(app, pane);
        job->rpath = remote_row_path(pane, entry_rel(pane, idx));
    } else if (in_archive(pane)) {
        job->kind   = QV_ARCHIVE;
        job->path   = pane.archive_path;
        job->member = member_raw(pane, entry_rel(pane, idx));
    } else {
        job->kind = QV_LOCAL;
        job->path = job->key;
    }
    return true;
}

// The text a quick view shows for a row: "" when the row has nothing to show,
// k_qv_reading while a fetch is on its way, and the words a file that is not
// text deserves. Everything is keyed by path, so a row that has been read once
// answers the next frame as fast as the last.
static std::string qv_preview(AppPanes* app, const Pane& pane, int idx) {
    QvJob job;
    if (!qv_job_for(app, pane, idx, &job)) return "";
    if (std::string* hit = qv_cache_find(app, job.key)) return *hit;

    if (job.kind == QV_LOCAL) {
        std::string body, err;
        qv_read_file(job.path, &body, &err);
        if (!err.empty()) return "(" + err + ")";
        if (!qv_to_text(&body)) body = k_qv_binary;
        qv_cache_put(app, job.key, body);
        return body;
    }

    for (std::unique_ptr<QvFetch>& f : app->qv_fetches)
        if (f->key == job.key) {
            // Already on its way. Say so to qv_reap(), and take back a stop that
            // was asked while the cursor was somewhere else: the worker may have
            // obeyed already, in which case it answers with a failure, nothing is
            // cached, and the next look at this name asks again.
            f->wanted = true;
            f->cancel = false;
            return k_qv_reading;
        }
    if (app->qv_fetches.size() < k_qv_inflight_max) qv_start(app, job);
    return k_qv_reading;
}

// Ctrl+Q: the inactive pane turns into a quick view of the file under the
// cursor in the active pane (TC's quick view panel). An archive member previews
// like any other file now; it is unpacked on a thread, one member at a time.
static void render_quick_view_panel(AppPanes* app, float w, float h, const char* id) {
    Pane& src = app->left_focus ? app->left : app->right;
    ImGui::BeginChild(id, ImVec2(w, h));
    if (src.selected < 0 || src.selected >= src.entry_count) {
        ImGui::TextDisabled("(nothing under the cursor)");
        ImGui::EndChild();
        return;
    }
    FileEntry* e = src.entries[src.selected];
    if (is_directory(e->name, e->mode)) {
        ImGui::TextDisabled("(directory: %s)", e->name);
        ImGui::EndChild();
        return;
    }
    std::string text = qv_preview(app, src, src.selected);
    ImGui::TextDisabled("%s", e->name);
    ImGui::Separator();
    ImGui::TextUnformatted(text.empty() ? "(nothing to preview)" : text.c_str());
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

    // Clicking anywhere in the pane - below the last row, between rows, on the
    // scrollbar - makes it the active pane. Checked on the child window, because
    // IsItemHovered() would refer to the widget before it.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0))
        app->left_focus = (&pane == &app->left);

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
                    app->left_focus = (&pane == &app->left);
                    ImGui::OpenPopup("ctxmenu");
                }
                if (clicked) {
                    pane.selected = i;
                    app->left_focus = (&pane == &app->left);
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
                app->left_focus = (&pane == &app->left);
                ImGui::OpenPopup("ctxmenu");
            }
            if (clicked) {
                pane.selected = i;
                app->left_focus = (&pane == &app->left);
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
        std::string text = qv_preview(app, pane, pane.selected);
        // A binary file on this disk still gets its hex dump. A member of an
        // archive and a file on a site are not on this disk to dump, and pulling
        // one down to find out is not what a preview is for.
        if (text == k_qv_binary && !in_archive(pane) && !in_remote(pane)) {
            static char hexbuf[16384];
            long dumped = fs_hex_dump(pane_row_path(pane, pane.selected).c_str(),
                                      hexbuf, sizeof(hexbuf), 2048);
            if (dumped < 0) ImGui::TextUnformatted("(unreadable)");
            else {
                ImGui::TextUnformatted("(binary - hex dump)");
                ImGui::TextUnformatted(hexbuf);
            }
        } else {
            ImGui::TextUnformatted(text.empty() ? "(nothing to preview)" : text.c_str());
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
    if (!open) close_viewer(app);
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
    char title[64];
    snprintf(title, sizeof(title), "About %s", MC_APP_NAME);
    ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse);
    // Who this build is, measured rather than typed: the toolchain versions and
    // the archive tools come from the running process.
    char ident[1024];
    app_about_identity(ident, sizeof(ident));
    ImGui::TextWrapped("%s", ident);
    // What the text is drawn with, and at what density. "The font is blurry" has
    // two unrelated causes - a build with no hinting, and glyphs baked for a
    // different monitor's density - and this line tells them apart.
    char raster[64];
    mc_font_rasterizer(raster, sizeof(raster));
    ImGui::TextWrapped("text: %s, %s, window scale %.2fx", raster,
                       mc_font_hinting_available() ? app->hinting : "no hinting in this build",
                       (double)app->display_scale);
    // Whether this build can reach a server at all, and over what. The site
    // manager says the same thing, but the About window is where a build without
    // libcurl gets noticed before anyone goes looking for the menu entry.
    char ftp[96];
    net_what(ftp, sizeof(ftp));
    ImGui::TextWrapped("remote: %s", ftp);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted(
        "TOTAL COMMANDER KEYS\n"
        "F1 help            F9 pane menu     F10 app menu    Alt+F4 quit\n"
        "F3 view            F4 edit          F5 copy         F6 move/rename\n"
        "F7 new folder      F8 / Del to the trash   Shift+F8 / Shift+Del direct delete\n"
        "Ins / Space        mark             Num+ / Num- extend / shrink\n"
        "Num* invert        Num/ restore     Ctrl+Num+ all   Ctrl+Num- none\n"
        "Alt+Num+/- mark or unmark the same extension\n"
        "Ctrl+A all         Ctrl+I other pane  Shift+arrows range mark\n"
        "Ctrl+F3/F4/F5/F6/F7 sort name / ext / date / size / unsorted\n"
        "Shift+F3 view only the entry under the cursor\n"
        "Tab switch pane    Enter open/run   Backspace or Ctrl+PgUp parent\n"
        "Ctrl+PgDn open entry under cursor  Alt+Left/Right dir history\n"
        "Ctrl+T new tab     Ctrl+W close     Ctrl+Shift+W close all tabs\n"
        "Ctrl+Tab / Ctrl+Shift+Tab cycle    Ctrl+1..9 jump to a tab\n"
        "Ctrl+Up / Ctrl+Shift+Down open the cursor dir in a new tab\n"
        "Ctrl+Left/Right show the cursor dir in the other pane\n"
        "Ctrl+U swap the pane directories   Ctrl+, go to the root\n"
        "Ctrl+C copy        Ctrl+X cut       Ctrl+V paste   Ctrl+P path into cmd line\n"
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
        "Ctrl+Shift+F8 tree in the panel   Shift+Tab split view: tree + list\n"
        "ARCHIVES (zip, tar, tar.gz/bz2/xz, 7z, RAR read-only)\n"
        "Enter on an archive browses it like a folder; .. or Backspace leaves it\n"
        "Alt+F5 pack        Alt+Shift+F5 pack and remove the sources\n"
        "Alt+F6 extract into the other pane   Alt+F9 extract into this one\n"
        "Alt+Shift+F9 test archive integrity  Ctrl+C/V copy members out\n"
        "The pack dialog picks format, level, password and 7z volumes; RAR and\n"
        "tar cannot be written with a password, and only 7z splits into volumes\n"
        "An archive's contents are read-only: extract, change, pack again\n\n"
        "$ command line     type a command, Enter runs it (cd, exit builtins)\n"
        "Alt+C prefill 'cd '   Alt+PgUp/PgDn command history   Esc clear\n\n"
        "MENUS\n"
        "F10 opens the Commands menu   Shift+F9 hotlist   Shift+F10 pane menu\n"
        "File, Commands, Options and Help sit on the menu bar\n"
        "Options > Fonts and theme... picks the font and the palette\n");
    if (!open) app->about_open = false;
    ImGui::End();
}

// Properties dialog (Alt+Enter): recursive count + size of the selected entry.
static void open_properties(AppPanes* app, Pane& pane) {
    if (in_remote(pane)) {
        // Counting a remote tree would mean downloading it. What the listing
        // already brought back - this row's mode, size and time - is what shows.
        if (pane.selected < 0 || pane.selected >= pane.entry_count) return;
        FileEntry* e = pane.entries[pane.selected];
        std::string vroot, vrpath;
        if (net_split_virtual(pane.current_dir, &vroot, &vrpath)) {
            std::string rpath = remote_row_path(pane, e->name);
            snprintf(app->props_path, sizeof(app->props_path), "%s", net_virtual_path(vroot, rpath).c_str());
        } else {
            snprintf(app->props_path, sizeof(app->props_path), "%s", pane.current_dir);
        }
        app->props_size = e->size;
        app->props_files = S_ISDIR(e->mode) ? 0 : 1;
        app->props_dirs = S_ISDIR(e->mode) ? 1 : 0;
        app->props_open = true;
        return;
    }
    if (in_archive(pane)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Properties need a directory (Backspace leaves the archive)");
        return;
    }
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
    // Focus the field itself. Offset -1 would mean the item submitted *before*
    // this call - the path bar above - so Ctrl+S looked like it did nothing.
    // The request is answered at the start of the next frame, against whichever
    // window holds focus by then, and a window that appears in between - the F3
    // preview, a dialog - takes it. So ask again until the field has the keyboard.
    if (pane.filter_focus_request) claim_focus(app);
    ImGui::PushID(id);
    ImGui::InputText("##filter", pane.filter, sizeof(pane.filter));
    if (ImGui::IsItemClicked()) app->left_focus = (&pane == &app->left);
    pane.filter_field_active = ImGui::IsItemActive();
    if (pane.filter_field_active) pane.filter_focus_request = false;
    ImGui::PopID();
    ImGui::PopStyleColor(2);
}

// "needle anywhere in hay", case-insensitively. The font filter is a substring
// box, not a glob: someone looking for DejaVu types "dejavu" and means that.
static bool icontains(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (!nl) return true;
    for (const char* p = hay; *p; p++)
        if (!strncasecmp(p, needle, nl)) return true;
    return false;
}

// The Options dialog: the settings that need a widget rather than a menu item.
// The font list is scanned once and filtered in place, because a system font
// tree holds hundreds of files and this still has to fit in a dialog.
static void render_options(AppPanes* app) {
    if (!app->options_open) return;
    if (!app->font_scanned) { app_scan_fonts(app->font_files); app->font_scanned = true; }

    // Never wider than the screen it opened on: the dialog is a column of long
    // absolute paths, and one that runs off the edge cannot be read or moved.
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(ImMin(620.0f, disp.x - 24.0f),
                                    ImMin(460.0f, disp.y - 24.0f)), ImGuiCond_FirstUseEver);
    bool open = app->options_open;
    ImGui::Begin("Options", &open);

    int count = 0;
    const char* const* names = mc_theme_list(&count);
    int tidx = 0;
    for (int i = 0; i < count; i++) if (!strcmp(app->theme, names[i])) { tidx = i; break; }
    if (ImGui::Combo("Theme", &tidx, names, count)) {
        snprintf(app->theme, sizeof(app->theme), "%s", names[tidx]);
        mc_apply_theme(app->theme);
        snprintf(app->status_msg, sizeof(app->status_msg), "Theme: %s", app->theme);
    }

    float size = app->font_size;
    if (ImGui::SliderFloat("Size", &size, 0.0f, 48.0f, "%.0f px")) {
        app->font_size = size < 0.0f ? 0.0f : size;
        app->font_dirty = true;
    }
    ImGui::TextWrapped("0 px is the built-in size, 13 px, for any font. The size is a "
                       "rasterization request and not a stretch: a TrueType font is drawn "
                       "from its outlines at the size asked for.");

    // Hinting sits next to the size because the two are one decision: a size says
    // how tall a glyph is, hinting says how it meets the pixel grid at that size.
    // A build without FreeType has no hinting to offer, and says which rasterizer
    // it has instead of showing a combo that cannot be honoured.
    ImGui::Spacing();
    int hc = 0;
    const char* const* hnames = mc_hinting_list(&hc);
    if (mc_font_hinting_available()) {
        int hidx = mc_hinting_index(app->hinting);
        if (hidx < 0) hidx = 0;
        if (ImGui::Combo("Hinting", &hidx, hnames, hc)) {
            snprintf(app->hinting, sizeof(app->hinting), "%s", hnames[hidx]);
            app->font_dirty = true;
            snprintf(app->status_msg, sizeof(app->status_msg), "Hinting: %s", app->hinting);
        }
        ImGui::TextWrapped("%s", mc_hinting_note(hidx));
    } else {
        char raster[64];
        mc_font_rasterizer(raster, sizeof(raster));
        ImGui::TextDisabled("Hinting is not offered: this build rasterizes text with %s, which does no "
                            "hinting at all. Build against freetype2 and the choice appears here.", raster);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Font");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##fontfilter", "filter the list",
                             app->font_filter, sizeof(app->font_filter));
    // The filter is a text field: while it holds the keyboard the arrows and
    // Delete edit the word, they do not walk the file list behind the dialog.
    app->options_field_active = ImGui::IsItemActive();

    const float frame = ImGui::GetFrameHeight();
    if (ImGui::BeginListBox("##fontlist", ImVec2(-FLT_MIN, frame * 12.0f))) {
        int cur = 0;   // 0 = the built-in font, i+1 = font_files[i]
        for (int i = 0; i < (int)app->font_files.size(); i++)
            if (app->font_files[i] == app->font) { cur = i + 1; break; }
        if (ImGui::Selectable("Built-in font", cur == 0)) {
            app->font[0] = '\0';
            app->font_dirty = true;
        }
        int shown = 0;
        for (int i = 0; i < (int)app->font_files.size(); i++) {
            const std::string& p = app->font_files[i];
            if (app->font_filter[0] && !icontains(p.c_str(), app->font_filter)) continue;
            shown++;
            if (ImGui::Selectable(file_base(p.c_str()), cur == i + 1)) {
                snprintf(app->font, sizeof(app->font), "%s", p.c_str());
                app->font_dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.c_str());
        }
        if (!shown) ImGui::TextDisabled("(no font file matches \"%s\")", app->font_filter);
        ImGui::EndListBox();
    }

    // What happened to the last pick, including the case where the file was
    // rejected and the built-in font went back in.
    if (app->font_msg[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.55f, 0.25f, 1.0f));
        ImGui::TextWrapped("%s", app->font_msg);
        ImGui::PopStyleColor();
    }
    // Wrapped, like everything else in the dialog: a sentence that is longer
    // than the window would otherwise push its frame off the screen.
    ImGui::TextWrapped("%d font files in the system and user font directories",
                       (int)app->font_files.size());
    ImGui::Spacing();
    if (ImGui::Button("Rescan")) app_scan_fonts(app->font_files);
    ImGui::SameLine();
    if (ImGui::Button("Close")) app->options_open = false;

    if (!open) app->options_open = false;
    ImGui::End();
}

// File menu: what TC puts under Files. Everything here acts on the entry under
// the cursor or on the marks, and anything that blocks goes through the deferred
// table instead of running inside the frame.
// ---------------------------------------------------------------------------
// Remote sites: the saved-site manager (Ctrl+F) and quick connect (Ctrl+N)
// ---------------------------------------------------------------------------

// The editor's fields are staged here rather than written straight into the site
// list: a half-typed host must not become the login a pane resolves, and Cancel
// has to mean nothing happened.
struct NetForm {
    char name[256];
    char host[256];
    char user[256];
    char pass[256];
    char path[1024];
    int  proto;
    int  port;
    bool remember;
    bool verify_tls;
    bool passive;
    bool open;                 // the editor is showing below the list
};
static NetForm g_net_form;

static void remote_form_blank(void) {
    g_net_form.name[0] = '\0';
    g_net_form.host[0] = '\0';
    g_net_form.user[0] = '\0';
    g_net_form.pass[0] = '\0';
    g_net_form.path[0] = '\0';
    g_net_form.proto = NET_FTP;
    g_net_form.port = 0;
    g_net_form.remember = false;
    g_net_form.verify_tls = true;
    g_net_form.passive = true;
    g_net_form.open = true;
}

static void remote_form_fill(const NetSite& s) {
    snprintf(g_net_form.name, sizeof(g_net_form.name), "%s", s.name.c_str());
    snprintf(g_net_form.host, sizeof(g_net_form.host), "%s", s.host.c_str());
    snprintf(g_net_form.user, sizeof(g_net_form.user), "%s", s.user.c_str());
    snprintf(g_net_form.pass, sizeof(g_net_form.pass), "%s", s.password.c_str());
    snprintf(g_net_form.path, sizeof(g_net_form.path), "%s", s.path.c_str());
    g_net_form.proto = (int)s.proto;
    g_net_form.port = s.port;
    g_net_form.remember = s.remember;
    g_net_form.verify_tls = s.verify_tls;
    g_net_form.passive = s.passive;
    g_net_form.open = true;
}

// Read the form back into a site. `base` carries over what the form has no field
// for (a remembered password the user did not retype). The host field also takes
// a whole URL, because pasting one there is the common mistake and the parser
// already knows how to unpack it.
static bool remote_form_site(AppPanes* app, NetSite* out, const NetSite* base) {
    *out = base ? *base : NetSite();
    out->name = g_net_form.name;
    out->proto = (NetProto)g_net_form.proto;
    out->host = g_net_form.host;
    out->port = g_net_form.port;
    out->user = g_net_form.user;
    if (g_net_form.pass[0]) out->password = g_net_form.pass;
    out->path = g_net_form.path;
    out->remember = g_net_form.remember;
    out->verify_tls = g_net_form.verify_tls;
    out->passive = g_net_form.passive;
    if (strchr(out->host.c_str(), ':') || strstr(out->host.c_str(), "/")) {
        NetSite parsed;
        std::string err;
        if (!net_parse_url(out->host.c_str(), &parsed, &err)) {
            snprintf(app->remote_note, sizeof(app->remote_note), "%s", err.c_str());
            return false;
        }
        out->host = parsed.host;
        if (parsed.port) out->port = parsed.port;
        if (!parsed.user.empty()) out->user = parsed.user;
        if (!parsed.path.empty()) out->path = parsed.path;
        out->proto = parsed.proto;
    }
    if (out->host.empty()) {
        snprintf(app->remote_note, sizeof(app->remote_note), "A site needs a host.");
        return false;
    }
    return true;
}

// Make a site findable by root URL, which is how a pane's virtual path gets its
// login back. Saved sites are already in the list; a quick connect goes into the
// session-only list, which is never written to the sites file.
static void remote_remember(AppPanes* app, const NetSite& site) {
    std::string root = net_site_root(site);
    for (NetSite& s : app->remote_adhoc)
        if (net_site_root(s) == root) { net_forget(&s); s = site; return; }
    for (NetSite& s : app->remote_sites)
        if (net_site_root(s) == root) { net_forget(&s); s = site; return; }
    app->remote_adhoc.push_back(site);
}

// Open a site in the focused pane. The listing itself happens in refresh_pane();
// this only decides where to go and puts the pane back if nothing came back,
// because an empty remote directory explains less than the directory the user
// was already looking at.
static void remote_open(AppPanes* app, const NetSite& site) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (!net_available()) {
        char what[128];
        net_what(what, sizeof(what));
        snprintf(app->status_msg, sizeof(app->status_msg), "%s", what);
        return;
    }
    std::string target = net_virtual_path(net_site_root(site), site.path);
    char back[4096];
    snprintf(back, sizeof(back), "%s", active.current_dir);
    remote_remember(app, site);
    navigate_to(app, &active, target.c_str());
    // A successful listing always has at least the ".." row, so nothing came
    // back only when the listing failed. refresh_pane() left the reason behind;
    // it reaches the status line from there.
    if (active.entry_count == 0) {
        navigate_to(app, &active, back);
        return;
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "%s", target.c_str());
    // A connect that worked closes the windows that offered it: a manager left
    // open over a pane that is already showing the site is a second window with
    // nothing left to decide. The failure paths above return early and leave
    // everything open, because there the reason is on screen and the fields are
    // what needs fixing.
    app->remote_mgr_open = false;
    app->remote_quick_open = false;
    app->remote_quick[0] = '\0';
    g_net_form.open = false;
    app->remote_edit = -1;
    app->remote_note[0] = '\0';
}

// Open a typed URL in the focused pane. Anything the URL leaves out comes from
// the site already known for that host, so "cd ftp.example.com/pub" after a
// logged-in visit does not ask the server for an anonymous session.
static void remote_open_url(AppPanes* app, const char* url) {
    NetSite site;
    std::string err;
    if (!net_parse_url(url, &site, &err)) {
        snprintf(app->status_msg, sizeof(app->status_msg), "%s", err.c_str());
        return;
    }
    const NetSite* known = net_site_for(net_site_root(site));
    if (known) {
        if (site.user.empty()) site.user = known->user;
        if (site.password.empty()) site.password = known->password;
        site.remember = known->remember;
        site.verify_tls = known->verify_tls;
        site.passive = known->passive;
    }
    remote_open(app, site);
}

static void remote_quick_commit(AppPanes* app) {
    NetSite site;
    std::string err;
    if (!net_parse_url(app->remote_quick, &site, &err)) {
        snprintf(app->remote_note, sizeof(app->remote_note), "%s", err.c_str());
        return;
    }
    // Typing "host/pub" is a shortcut to a saved site, not a request to log in
    // anonymously: whatever the saved entry knows fills the gaps the URL left.
    const NetSite* saved = net_sites_find(app->remote_sites, net_site_root(site));
    if (saved) {
        if (site.user.empty()) site.user = saved->user;
        if (site.password.empty()) site.password = saved->password;
        site.remember = saved->remember;
        site.verify_tls = saved->verify_tls;
        site.passive = saved->passive;
    }
    app->remote_quick_open = false;
    app->remote_quick[0] = '\0';
    remote_open(app, site);
}
static void remote_save_form(AppPanes* app) {
    const NetSite* base = (app->remote_edit >= 0 &&
                           app->remote_edit < (int)app->remote_sites.size())
                              ? &app->remote_sites[app->remote_edit] : nullptr;
    NetSite site;
    if (!remote_form_site(app, &site, base)) return;   // the reason is in remote_note
    if (base) {
        net_forget(base);            // an old login must not be reused after an edit
        app->remote_sites[app->remote_edit] = site;
    } else {
        app->remote_sites.push_back(site);
        app->remote_edit = (int)app->remote_sites.size() - 1;
    }
    remote_sites_write(app);
    snprintf(app->status_msg, sizeof(app->status_msg), "Saved site %s",
             site.name.empty() ? net_site_root(site).c_str() : site.name.c_str());
}

// One row of the site manager's list. The saved sites and the connections this
// session made with Ctrl+N live in two vectors, and only one of them is written
// back to the sites file, but to the arrow keys they are one list: the site you
// connected to a moment ago is the one you are most likely to want next.
struct NetRow {
    bool adhoc;   // false = saved site at idx, true = this session's quick connect
    int  idx;
};

static std::vector<NetRow> net_rows(const AppPanes* app) {
    std::vector<NetRow> rows;
    for (int i = 0; i < (int)app->remote_sites.size(); i++) rows.push_back({ false, i });
    for (int i = 0; i < (int)app->remote_adhoc.size(); i++) rows.push_back({ true, i });
    return rows;
}

// Set when the keyboard moved the manager's cursor, so the row it landed on is
// scrolled into view. The mouse does not set it: a click already put the row
// under the pointer, and scrolling on a click would move the list the user is
// looking at.
static bool g_net_cursor_moved = false;

// Move the manager's cursor over its list of connections. From no cursor at all
// Down enters at the top and Up at the bottom, which is how the panes' own cursor
// behaves; past either end the cursor stops instead of wrapping, because a
// cursor that wraps in a three-row list is a way to lose the row you were on.
// A key that is not a navigation key leaves the cursor where it is, which is how
// the caller tells the difference between taking the key and dropping it.
static int net_cursor_step(int cur, int count, SDL_Keycode key) {
    if (count <= 0) return -1;
    switch (key) {
    case SDLK_DOWN:
        return cur < 0 ? 0 : (cur + 1 < count ? cur + 1 : count - 1);
    case SDLK_UP:
        return cur < 0 ? count - 1 : (cur > 0 ? cur - 1 : 0);
    case SDLK_HOME:
        return 0;
    case SDLK_END:
        return count - 1;
    case SDLK_PAGEUP:
        return (cur < 0 || cur - 10 < 0) ? 0 : cur - 10;
    case SDLK_PAGEDOWN:
        return cur < 0 ? (count - 1 < 10 ? count - 1 : 9)
                       : (cur + 10 < count ? cur + 10 : count - 1);
    default:
        return cur;
    }
}

static void render_remote_manager(AppPanes* app) {
    if (!app->remote_mgr_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 380, io.DisplaySize.y * 0.5f - 260),
                            ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Remote sites (Ctrl+F)", &open, ImGuiWindowFlags_NoCollapse);

    char what[160];
    net_what(what, sizeof(what));
    if (!net_available()) {
        // The dialog still opens: a build without the transport says so instead
        // of offering a connect that cannot work.
        ImGui::TextUnformatted(what);
        ImGui::Spacing();
        ImGui::TextDisabled("Install the libcurl development package and rebuild to browse "
                            "FTP, FTPS and SFTP sites. Everything else in the program is "
                            "unchanged.");
        if (app->remote_note[0])
            ImGui::TextDisabled("%s", app->remote_note);
        if (!open) app->remote_mgr_open = false;
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s", what);
    ImGui::Separator();

    if (app->remote_sites.empty() && app->remote_adhoc.empty())
        ImGui::TextUnformatted("(no saved sites - add one below, or press Ctrl+N to connect once)");

    std::vector<NetRow> rows = net_rows(app);
    // Removing a site, or a promote moving one out of the session list, can leave
    // the cursor past the end of a list that got shorter. It lands on the last row
    // rather than on nothing, so the keyboard never loses its place.
    if (app->remote_cursor >= (int)rows.size())
        app->remote_cursor = rows.empty() ? -1 : (int)rows.size() - 1;

    int remove_idx = -1;
    int promote_idx = -1;
    bool adhoc_header = false;
    for (int r = 0; r < (int)rows.size(); r++) {
        const NetRow& row = rows[r];
        const NetSite& s = row.adhoc ? app->remote_adhoc[row.idx] : app->remote_sites[row.idx];
        if (row.adhoc && !adhoc_header) {
            adhoc_header = true;
            ImGui::TextDisabled("connected this session (not saved)");
        }
        ImGui::PushID(row.adhoc ? 1 : 0);
        ImGui::PushID(row.idx);
        std::string root = net_site_root(s);
        char label[768];
        snprintf(label, sizeof(label), "%s  %s%s", s.name.empty() ? root.c_str() : s.name.c_str(),
                 s.name.empty() ? "" : root.c_str(),
                 s.verify_tls || s.proto == NET_SFTP ? "" : "  [certificate not verified]");
        // The row has to stop where the buttons begin. A Selectable with no size
        // spans the whole content width, so it sat on top of all three buttons and
        // took their click: pressing "connect" only ever selected the row, which
        // is the same as pressing "edit". Sizing the row is the fix — the order the
        // widgets are submitted in does not settle who owns the pixel.
        float buttons_x = ImGui::GetWindowWidth() - 190.0f;
        float row_w = buttons_x - ImGui::GetCursorPosX();
        if (row_w < 40.0f) row_w = 40.0f;
        if (ImGui::Selectable(label, app->remote_cursor == r, 0, ImVec2(row_w, 0))) {
            app->remote_cursor = r;
            // A session connection has no saved entry to edit, so the form opens
            // as a new site carrying its fields, which is how a quick connect
            // becomes a saved one.
            app->remote_edit = row.adhoc ? -1 : row.idx;
            remote_form_fill(s);
        }
        if (g_net_cursor_moved && app->remote_cursor == r)
            ImGui::SetScrollHereY(0.5f);
        ImGui::SameLine(buttons_x);
        if (ImGui::SmallButton("connect")) { app->remote_cursor = r; remote_open(app, s); }
        ImGui::SameLine();
        if (ImGui::SmallButton("edit")) {
            app->remote_cursor = r;
            app->remote_edit = row.adhoc ? -1 : row.idx;
            remote_form_fill(s);
        }
        ImGui::SameLine();
        if (row.adhoc) {
            if (ImGui::SmallButton("save")) promote_idx = row.idx;
        } else {
            if (ImGui::SmallButton("remove")) remove_idx = row.idx;
        }
        ImGui::PopID();
        ImGui::PopID();
    }
    g_net_cursor_moved = false;
    if (promote_idx >= 0) {
        // "save" on a session connection fills the form with what was just used
        // and leaves it as a new site, so the name and the password are chosen in
        // the fields rather than guessed at by the button.
        app->remote_edit = -1;
        remote_form_fill(app->remote_adhoc[promote_idx]);
    }
    if (remove_idx >= 0) {
        net_forget(&app->remote_sites[remove_idx]);
        app->remote_sites.erase(app->remote_sites.begin() + remove_idx);
        if (app->remote_edit == remove_idx) { app->remote_edit = -1; g_net_form.open = false; }
        else if (app->remote_edit > remove_idx) app->remote_edit--;
        remote_sites_write(app);
    }

    ImGui::Separator();
    if (!g_net_form.open) {
        if (ImGui::Button("Add site")) { app->remote_edit = -1; remote_form_blank(); }
        ImGui::SameLine();
        if (ImGui::Button("Quick connect")) {
            app->remote_quick_open = true;
            app->remote_focus_request = true;
        }
    } else {
        ImGui::TextUnformatted(app->remote_edit >= 0 ? "Edit site" : "New site");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("name", g_net_form.name, sizeof(g_net_form.name));
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("host", g_net_form.host, sizeof(g_net_form.host));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A bare host, or a whole URL: ftp://host, ftps://host:990, sftp://host");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("port", &g_net_form.port, 0, 0);
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("user", g_net_form.user, sizeof(g_net_form.user));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("password", g_net_form.pass, sizeof(g_net_form.pass),
                         ImGuiInputTextFlags_Password);
        ImGui::SetNextItemWidth(420);
        ImGui::InputText("start in", g_net_form.path, sizeof(g_net_form.path));
        const char* proto_names[] = { "FTP (upgrade if offered)", "FTPS (explicit TLS)",
                                      "FTPS (implicit, port 990)", "SFTP (SSH)" };
        ImGui::SetNextItemWidth(300);
        ImGui::Combo("protocol", &g_net_form.proto, proto_names, IM_ARRAYSIZE(proto_names));
        ImGui::Checkbox("passive mode", &g_net_form.passive);
        if (g_net_form.proto != NET_SFTP) {
            ImGui::SameLine();
            ImGui::Checkbox("verify the certificate", &g_net_form.verify_tls);
            if (!g_net_form.verify_tls)
                ImGui::TextDisabled("certificate not verified: anyone on the path can pose as "
                                    "this server");
        } else {
            ImGui::TextDisabled("SFTP trusts the server's host key the first time it is seen");
        }
        ImGui::Checkbox("remember this password", &g_net_form.remember);
        if (g_net_form.remember)
            ImGui::TextDisabled("written as plain text in %s, readable only by you",
                                net_sites_path().c_str());
        if (ImGui::Button("Connect")) {
            NetSite site;
            const NetSite* base = (app->remote_edit >= 0 &&
                                   app->remote_edit < (int)app->remote_sites.size())
                                      ? &app->remote_sites[app->remote_edit] : nullptr;
            if (remote_form_site(app, &site, base)) remote_open(app, site);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save")) remote_save_form(app);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { g_net_form.open = false; app->remote_edit = -1; app->remote_note[0] = '\0'; }
    }

    if (app->remote_note[0]) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", app->remote_note);
    }
    if (!open) app->remote_mgr_open = false;
    ImGui::End();
}

// Ctrl+N: one line, one connection, nothing saved. The same text the command
// line takes after "cd ", so there is one grammar for a typed address.
static void render_remote_quick(AppPanes* app) {
    if (!app->remote_quick_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(620, 200), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 310, io.DisplaySize.y * 0.5f - 100),
                            ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Quick connect (Ctrl+N)", &open, ImGuiWindowFlags_NoCollapse);
    char what[160];
    net_what(what, sizeof(what));
    if (!net_available()) {
        ImGui::TextUnformatted(what);
        ImGui::TextDisabled("Install the libcurl development package and rebuild.");
        if (!open) app->remote_quick_open = false;
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("host, user@host, user@host:port/path, ftp://... ftps://... sftp://...");
    if (app->remote_focus_request) claim_focus(app);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##quick", app->remote_quick, sizeof(app->remote_quick));
    if (ImGui::Button("Connect")) remote_quick_commit(app);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) { app->remote_quick_open = false; app->remote_quick[0] = '\0'; }
    if (app->remote_note[0])
        ImGui::TextDisabled("%s", app->remote_note);
    if (!open) app->remote_quick_open = false;
    ImGui::End();
}

static void render_file_items(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;

    if (ImGui::MenuItem("View", "F3")) active.quick_view = !active.quick_view;
    if (ImGui::MenuItem("Open with the default application", "Alt+F3")) open_cursor_default(app);
    if (ImGui::MenuItem("Edit", "F4")) edit_cursor(app);
    ImGui::Separator();
    if (ImGui::MenuItem("New folder", "F7")) open_name_dialog(app, OP_MKDIR, nullptr);
    if (ImGui::MenuItem("New file", "Shift+F4")) open_name_dialog(app, OP_NEWFILE, nullptr);
    if (ImGui::MenuItem("Rename", "F2")) rename_cursor(app);
    // Copy and move stop at the destination dialog; the operation itself runs
    // between frames.
    if (ImGui::MenuItem("Copy", "F5")) app->pending_op = DEFER_COPY;
    if (ImGui::MenuItem("Move / rename", "F6")) app->pending_op = DEFER_MOVE;
    if (ImGui::MenuItem("Copy in place", "Shift+F5")) app->pending_op = DEFER_COPY_IN_PLACE;
    if (ImGui::MenuItem("Rename in place", "Shift+F6")) rename_cursor(app);
    if (ImGui::MenuItem("Link in other pane", "Shift+Ctrl+F5")) app->pending_op = DEFER_SYMLINK_OTHER;
    ImGui::Separator();
    // The internal clipboard, next to the copy and move above it: TC keeps both
    // pairs under Files, and "Copy" with two meanings in one menu is what the
    // shortcuts are for.
    if (ImGui::MenuItem("Copy marked", "Ctrl+C")) clipboard_set(app, active, false);
    if (ImGui::MenuItem("Cut marked", "Ctrl+X")) clipboard_set(app, active, true);
    if (ImGui::MenuItem("Paste", "Ctrl+V")) app->pending_op = DEFER_PASTE;
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "F8 / Del")) request_delete(app, false);
    if (ImGui::MenuItem("Delete without the trash", "Shift+F8")) request_delete(app, true);
    ImGui::Separator();
    if (ImGui::MenuItem("Properties", "Alt+Enter")) open_properties(app, active);
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Alt+F4")) app->quit_request = true;
}

// Options menu: the settings that outlive the current directory. TC keeps these
// under Config; ours are few enough for one menu, and the two that need a widget
// (a theme picker, a font picker) open the Options dialog.
static void render_options_items(AppPanes* app) {
    if (ImGui::MenuItem("Fonts and theme...")) app->options_open = !app->options_open;

    if (ImGui::BeginMenu("Theme")) {
        int count = 0;
        const char* const* names = mc_theme_list(&count);
        for (int i = 0; i < count; i++) {
            if (ImGui::MenuItem(names[i], nullptr, !strcmp(app->theme, names[i]))) {
                snprintf(app->theme, sizeof(app->theme), "%s", names[i]);
                mc_apply_theme(app->theme);
                snprintf(app->status_msg, sizeof(app->status_msg), "Theme: %s", app->theme);
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Show hidden files", "Alt+.", app->show_hidden)) {
        app->show_hidden = !app->show_hidden;
        fs_set_show_hidden(app->show_hidden);
        app_panes_refresh_left(app);
        app_panes_refresh_right(app);
    }
    if (ImGui::MenuItem("Define columns", "Shift+F1")) app->columns_open = !app->columns_open;

    if (ImGui::BeginMenu("Sort by")) {
        static const int         keys[5] = { FS_SORT_NAME, FS_SORT_EXT, FS_SORT_DATE, FS_SORT_SIZE, FS_SORT_NONE };
        static const char*       names[5] = { "Name", "Extension", "Date", "Size", "Unsorted" };
        static const char*       hints[5] = { "Ctrl+F3", "Ctrl+F4", "Ctrl+F5", "Ctrl+F6", "Ctrl+F7" };
        static const char*       short_[5] = { "name", "ext", "date", "size", "unsorted" };
        for (int i = 0; i < 5; i++) {
            if (ImGui::MenuItem(names[i], hints[i], fs_get_sort_mode() == keys[i])) {
                fs_set_sort_mode(keys[i]);
                snprintf(app->status_msg, sizeof(app->status_msg), "Sort by: %s", short_[i]);
                app_panes_refresh_left(app);
                app_panes_refresh_right(app);
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    // The same switch the delete prompt reports, so the menu says what Del will do.
    if (ImGui::MenuItem("Send deletions to the trash", nullptr, app->trash_mode != 0)) {
        app->trash_mode = app->trash_mode ? 0 : 1;
        snprintf(app->status_msg, sizeof(app->status_msg), "Deletes %s%s",
                 app->trash_mode ? "go to the trash" : "are direct",
                 (app->trash_mode && !fs_trash_available())
                     ? " (none found on this system)" : "");
    }
}

// Commands menu items. Shared by the menu bar and the F10 keyboard menu so the
// two can never drift apart.
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
    if (ImGui::MenuItem("Remote sites", "Ctrl+F")) {
        app->remote_mgr_open = !app->remote_mgr_open;
        app->remote_note[0] = '\0';
    }
    if (ImGui::MenuItem("Quick connect", "Ctrl+N")) {
        app->remote_quick_open = !app->remote_quick_open;
        app->remote_focus_request = true;
        app->remote_note[0] = '\0';
    }
    if (ImGui::MenuItem("Visited directories", "Alt+Down")) app->dirhist_open = !app->dirhist_open;
    if (ImGui::MenuItem("Command history", "Alt+F8")) app->cmdhist_open = !app->cmdhist_open;
    if (ImGui::MenuItem("Left drive", "Alt+F1")) { app->volumes_open = true; app->volumes_target = 0; }
    if (ImGui::MenuItem("Right drive", "Alt+F2")) { app->volumes_open = true; app->volumes_target = 1; }
    if (ImGui::MenuItem("Directory tree", "Alt+F10")) {
        if (app->tree_open) app->tree_open = false; else tree_open(app, active);
    }
    if (ImGui::MenuItem("Tree in this panel", "Ctrl+Shift+F8")) tree_mode_toggle(app, active, TREE_EMBED);
    if (ImGui::MenuItem("Split view: tree + list", "Shift+Tab")) tree_mode_toggle(app, active, TREE_SPLIT);
    if (ImGui::MenuItem("Pack archive", "Alt+F5")) do_pack(app, false);
    // Anything below that blocks runs between frames, not here.
    if (ImGui::MenuItem("Extract archive", "Alt+F6")) app->pending_op = DEFER_EXTRACT;
    if (ImGui::MenuItem("Count marked", "Ctrl+L")) count_marked_sizes(app);
    if (ImGui::MenuItem("Fill directory sizes", "Alt+Shift+Enter")) app->pending_op = DEFER_FILL_SIZES;
    if (ImGui::MenuItem("Add to hotlist", "Alt+D")) {
        bool dup = false;
        for (const std::string& s : app->hotlist) if (s == active.current_dir) { dup = true; break; }
        if (!dup) { app->hotlist.push_back(active.current_dir); hotlist_save(app); }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Mark all", "Ctrl+Num+")) select_all_marks(active);
    if (ImGui::MenuItem("Clear marks", "Ctrl+Num-")) clear_marks(active);
    if (ImGui::MenuItem("Invert marks", "Num*")) invert_marks(active);
    if (ImGui::MenuItem("Switch to other pane", "Ctrl+I")) app->left_focus = !app->left_focus;
    if (ImGui::MenuItem("Current path to command line", "Ctrl+P")) cmd_put_path(app, active);
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
}

void app_panes_render(AppPanes* app) {
    ImGuiIO& io = ImGui::GetIO();
    bool& left_focus = app->left_focus;
    Pane& left = app->left;
    Pane& right = app->right;

    // A transfer that finished since the last frame gets its panes refreshed and
    // its result in the status bar here, and a row the user dismissed is joined
    // and dropped. Between frames, because joining a thread is not something to
    // do inside one - and because the status line it writes is read just below.
    bg_reap(app);

    // Same for the quick view: a preview that arrived from a site or an archive
    // is filed under the path it was asked for here, and its thread is joined
    // here, where waiting for a finished thread costs nothing.
    qv_reap(app);

    // ImGui answers one keyboard-focus request per frame, and the last one
    // submitted wins. The command line at the bottom asks on every frame, so any
    // field that wants the caret has to claim it here and be left alone after.
    app->focus_claimed = false;

    // A listing that failed did so inside refresh_pane(), which has no status
    // line of its own. The reason waits there and lands here, where the user
    // sees it instead of an empty pane.
    if (!g_net_note.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "%s", g_net_note.c_str());
        g_net_note.clear();
    }

    // Fullscreen main window (MC-style, no decorations)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Midnight Commander", nullptr,
                 ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    // Top menu bar. BeginMenuBar() draws nothing unless the window carries
    // ImGuiWindowFlags_MenuBar, so the flag belongs to this Begin, not here.
    // The Commands items also back the F10 keyboard menu.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            render_file_items(app);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Commands")) {
            render_commands_items(app);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Options")) {
            render_options_items(app);
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
    // The transfer strip, when there are transfers. Its rows come out of the
    // panes' height, which is why the room is measured before the panes are laid
    // out and not after they have taken all of it.
    const float xfer_h   = bg_strip_height(app, frame);
    const float tabbar_h = frame;         // tab strip
    const float title_h  = frame;         // pane path bar
    const float sep_h    = 6.0f;          // separator under the path bar
    const float filt_h   = frame + 2.0f;  // quick filter row, when shown
    const float win_h    = ImGui::GetWindowHeight();
    const float table_h  = win_h - ImGui::GetCursorPosY() - bar_h - xfer_h;
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
    render_tab_bar(app, left, "ltabs");
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
    if (left.path_focus_request) claim_focus(app);
    ImGui::InputText("##leftpath", left.path_edit, sizeof(left.path_edit));
    left.path_edit_active = ImGui::IsItemActive();
    if (left.path_edit_active) left.path_focus_request = false;
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
    render_tab_bar(app, right, "rtabs");
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
    if (right.path_focus_request) claim_focus(app);
    ImGui::InputText("##rightpath", right.path_edit, sizeof(right.path_edit));
    right.path_edit_active = ImGui::IsItemActive();
    if (right.path_edit_active) right.path_focus_request = false;
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

    // The transfer list, between the panes and the command line. It draws
    // nothing at all while no transfer is running or remembered.
    render_transfers(app, xfer_h);

    // Bottom bar: pinned to the window bottom so it can never overflow. The op
    // prompt used to live here as a one-line field; the operations are real
    // dialogs now, so the bar is the command line again.
    ImGui::SetCursorPosY(win_h - bar_h);
    ImGui::Separator();
    {
        // Command line (MC-style: always focused). Status messages show as
        // dimmed text after the input while the command is empty.
        // With no status message the line reports marks and the clipboard,
        // like the TC status line.
        char info[128];
        info[0] = '\0';
        if (app->cmd[0] == '\0' && app->status_msg[0] == '\0') {
            Pane& act = left_focus ? left : right;
            int marks = marked_count(act);
            // A site reached without checking its certificate has to be said out
            // loud where the user is actually sitting. The site list says it, but a
            // pane can be browsing that site with nothing on screen reminding anyone
            // that anyone on the path could be posing as the server. It outranks the
            // mark count for exactly that reason.
            const NetSite* unsite = in_remote(act) ? net_site_for(act.remote_root) : nullptr;
            if (unsite && !unsite->verify_tls)
                snprintf(info, sizeof(info), "certificate not verified for %s", act.remote_root);
            else if (!app->clipboard.paths.empty())
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
        bool allow_cmd_focus = (app->confirm == CONF_NONE) && (app->active_path_pane < 0) &&
                               !app->search_input_active && (app->op == OP_NONE) &&
                               !left.filter_field_active && !right.filter_field_active &&
                               !app->options_field_active;
        // Offset 0 means this field. With -1 the request resolved to whatever
        // item came before it, which was nothing focusable, so the command line
        // was never the active item and any earlier request got thrown away.
        // It still has to ask politely: a request lands on the *next* frame, and
        // if a press is in flight it takes ActiveId away from whatever is being
        // clicked, so the release finds a different item and the click is
        // dropped. That is what made the menu bar titles open but their items do
        // nothing. So: only while the keyboard is genuinely free.
        bool keyboard_free = !app->focus_claimed && (ImGui::GetActiveID() == 0) &&
                             !ImGui::IsPopupOpen((ImGuiID)0, ImGuiPopupFlags_AnyPopup);
        if (allow_cmd_focus && keyboard_free)
            ImGui::SetKeyboardFocusHere();
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
    render_pack_dialog(app);
    render_op_dialog(app);
    render_tree(app);
    render_search(app);
    render_confirm(app);
    render_about(app);
    render_properties(app);
    render_options(app);
    render_remote_manager(app);
    render_remote_quick(app);
}

AppPanes* app_panes_init() {
    AppPanes* app = new AppPanes();
    const char* home = getenv("HOME");
    if (!home || !*home) home = "/";

    // Before the first refresh: a pane that navigates to a site needs the login,
    // and refresh_pane() reaches the site list through these bindings.
    net_bind(app);
    remote_sites_load(app);

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
    // The Options dialog edits these live, so what the session showed is what the
    // next one starts with.
    snprintf(cfg->theme, sizeof(cfg->theme), "%s", app->theme);
    snprintf(cfg->font, sizeof(cfg->font), "%s", app->font);
    cfg->font_size = app->font_size;
    snprintf(cfg->hinting, sizeof(cfg->hinting), "%s", app->hinting);
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
// Can a restored tab still be opened? The question has to be asked of the right
// thing, because a tab's path is not always a filesystem path. A site tab and an
// archive tab are virtual: "ftp://host/dir" and "pkg.zip::sub" are names the
// filesystem has never heard of, so checking them with stat() fails and a session
// that ended on a site or inside an archive came back with those tabs quietly
// thrown away - the pane landed on the startup directory instead, and the copy
// the user then started looked like an ordinary local one. A site tab is kept
// whatever the server is doing right now: the login is looked up when the pane is
// refreshed, and a site that cannot be reached says so in the status line rather
// than taking the user's place back from them. An archive tab is kept while the
// archive file itself is still on disk.
static bool tab_dir_still_there(const char* path) {
    struct stat st;
    std::string vroot, vpath;
    if (net_split_virtual(path, &vroot, &vpath)) return true;
    char archive[4096], prefix[1024];
    if (arch_split_virtual(path, archive, sizeof archive, prefix, sizeof prefix))
        return stat(archive, &st) == 0 && S_ISREG(st.st_mode);
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void app_panes_apply_state(AppPanes* app, const MCConfig* cfg) {
    if (cfg->view_mode >= 0 && cfg->view_mode <= 2) app->view_mode = cfg->view_mode;
    if (cfg->theme[0]) snprintf(app->theme, sizeof(app->theme), "%s", cfg->theme);
    snprintf(app->font, sizeof(app->font), "%s", cfg->font);
    app->font_size = cfg->font_size > 0.0f ? cfg->font_size : 0.0f;   // 0 = the built-in size
    // A hinting name the build does not know is dropped rather than guessed at:
    // the default mode is the one the loader would have chosen anyway.
    if (mc_hinting_index(cfg->hinting) >= 0)
        snprintf(app->hinting, sizeof(app->hinting), "%s", cfg->hinting);
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
            if (!tab_dir_still_there(tabs[t])) continue;
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
    // A transfer thread is inside libcurl with a handle of its own. Ask it to
    // stop, wait for it to finish the file it is on, and only then release the
    // connections and the library, which is what net_shutdown() does.
    bg_stop_all(app);
    // Quick view fetches are inside the same transport, so they go the same way.
    qv_stop_all(app);
    free_entries(app->left.entries, app->left.entry_count);
    free_entries(app->right.entries, app->right.entry_count);
    // The cached handles belong to this app's site list, and so do the pointers
    // into it: both have to go before the AppPanes does.
    net_bind(nullptr);
    net_shutdown();
    delete app;
}

// Split a typed path into the directory part and the name part.
static void split_path(const std::string& path, std::string* dir, std::string* base) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) { dir->clear(); *base = path; return; }
    if (slash == 0) { *dir = "/"; *base = path.substr(1); return; }
    *dir = path.substr(0, slash);
    *base = path.substr(slash + 1);
}

// A destination typed with a trailing slash is a promise about a directory, so
// creating it is what the user asked for. Without the slash the directory has to
// be there already: a typo in a typed path must not quietly make a second one.
static bool make_dest_dirs(const std::string& path) {
    size_t i = 0;
    while (i <= path.size()) {
        size_t slash = path.find('/', i);
        std::string head = (slash == std::string::npos) ? path : path.substr(0, slash);
        struct stat st;
        if (!head.empty() && (stat(head.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) &&
            !fs_make_dir(head.c_str()))
            return false;
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return true;
}

// Read the dialog's field as a destination and turn it into the plan's
// destination directory plus an optional name. The field means one of two
// things and the difference has to be obvious: a directory (every entry keeps
// its own name) or the target itself (one entry, renamed). Anything that does
// not resolve gets a reason in the dialog instead of a status line the user has
// already stopped reading.
static bool resolve_destination(AppPanes* app) {
    OpPlan& plan = app->op_plan;
    char value[4096];
    snprintf(value, sizeof(value), "%s", app->op_name);
    size_t len = strlen(value);
    while (len && value[len - 1] == ' ') value[--len] = '\0';   // trailing spaces are never meant
    if (!len) {
        snprintf(app->op_error, sizeof(app->op_error), "Enter a destination.");
        return false;
    }

    // Coming out of an archive, the field can only be a directory: the members
    // keep the paths they have inside the archive, and a folder row names a
    // whole subtree, which has no single new name to be given.
    struct stat st;
    bool slash = value[len - 1] == '/';
    std::string vroot, vrpath;
    bool dst_remote = net_split_virtual(value, &vroot, &vrpath);
    if (dst_remote) {
        // A remote destination names either a directory on the server or the name
        // one entry lands under, and only the server can tell them apart. One
        // MLST decides it; a site root is always a directory.
        const NetSite* site = remote_site_for_root(app, vroot);
        if (!site) return false;
        std::string rpath = vrpath;
        while (rpath.size() > 1 && rpath.back() == '/') rpath.pop_back();
        FileEntry me;
        std::string err;
        bool is_dir = rpath.empty() || (net_stat(*site, rpath, &me, &err) && S_ISDIR(me.mode));
        if (slash || is_dir) {
            plan.dst_dir = net_virtual_path(vroot, rpath);
            plan.name_override.clear();
        } else if (plan.names.size() == 1) {
            size_t at = rpath.find_last_of('/');
            std::string dir = at == std::string::npos ? "" : rpath.substr(0, at);
            std::string base = at == std::string::npos ? rpath : rpath.substr(at + 1);
            if (base.empty()) {
                snprintf(app->op_error, sizeof(app->op_error), "That destination has no name in it.");
                return false;
            }
            plan.dst_dir = net_virtual_path(vroot, dir);
            plan.name_override = base;
        } else {
            snprintf(app->op_error, sizeof(app->op_error),
                     "%d entries are selected, so the destination has to be a directory.",
                     (int)plan.names.size());
            return false;
        }
    } else if (!plan.archive.empty() || slash || (stat(value, &st) == 0 && S_ISDIR(st.st_mode))) {
        plan.dst_dir = (slash && len > 1) ? std::string(value).substr(0, len - 1) : value;
        plan.name_override.clear();
        if (slash && stat(plan.dst_dir.c_str(), &st) != 0) {
            if (!make_dest_dirs(plan.dst_dir)) {
                snprintf(app->op_error, sizeof(app->op_error), "Cannot create %s: %s",
                         plan.dst_dir.c_str(), strerror(errno));
                return false;
            }
        }
    } else if (plan.names.size() == 1) {
        std::string dir, base;
        split_path(value, &dir, &base);
        if (base.empty()) {
            snprintf(app->op_error, sizeof(app->op_error), "That destination has no name in it.");
            return false;
        }
        if (dir.empty()) dir = ".";
        plan.dst_dir = dir;
        plan.name_override = base;
    } else {
        snprintf(app->op_error, sizeof(app->op_error),
                 "%d entries are selected, so the destination has to be a directory.",
                 (int)plan.names.size());
        return false;
    }

    if (!dst_remote) {
        if (stat(plan.dst_dir.c_str(), &st) != 0) {
            snprintf(app->op_error, sizeof(app->op_error), "There is no directory %s.", plan.dst_dir.c_str());
            return false;
        }
        if (!S_ISDIR(st.st_mode)) {
            snprintf(app->op_error, sizeof(app->op_error), "%s is a file, not a directory.",
                     plan.dst_dir.c_str());
            return false;
        }
        char arch[4096], prefix[1024];
        if (arch_split_virtual(plan.dst_dir.c_str(), arch, sizeof(arch), prefix, sizeof(prefix))) {
            snprintf(app->op_error, sizeof(app->op_error), "An archive cannot be written into.");
            return false;
        }
    }
    // Copying a file onto itself loses it: the copy opens the target for writing
    // before it reads the source. A move that names its own source does nothing.
    // Comparing per entry rather than per directory also catches the case where
    // the destination is spelled differently but means the same file.
    if (plan.archive.empty()) {
        for (size_t i = 0; i < plan.names.size(); i++) {
            if (plan.src_dir + "/" + plan.names[i] != plan_target(plan, i)) continue;
            snprintf(app->op_error, sizeof(app->op_error),
                     "%s is already there. Type a different name, or a different folder.",
                     plan.names[i].c_str());
            return false;
        }
    }
    return true;
}

void app_panes_commit_op(AppPanes* app) {
    if (app->op == OP_NONE) return;

    if (app->op == OP_COPY || app->op == OP_MOVE) {
        if (!resolve_destination(app)) return;      // the dialog stays up, with the reason
        OpPlan& plan = app->op_plan;
        int existing = plan_existing(plan);
        if (existing > 0) {
            app->confirm = plan.is_move ? CONF_OVERWRITE_MOVE
                         : (!plan.archive.empty() ? CONF_OVERWRITE_EXTRACT : CONF_OVERWRITE);
            snprintf(app->confirm_text, sizeof(app->confirm_text),
                     "%d target%s in %s already exist%s. Overwrite?", existing,
                     existing == 1 ? "" : "s", plan.dst_dir.c_str(), existing == 1 ? "s" : "");
        }
        app->op = OP_NONE;
        app->op_name[0] = '\0';
        if (!existing) app->pending_op = DEFER_OP_RUN;   // the confirm's Yes runs the same plan
        return;
    }

    Pane& active = app->left_focus ? app->left : app->right;
    // Rename, mkdir and new file build a path under the folder captured when the
    // dialog opened, so they cannot run on archive members.
    const std::string dir = app->op_plan.src_dir.empty() ? std::string(active.current_dir)
                                                         : app->op_plan.src_dir;
    char virt_archive[4096], virt_prefix[4096];
    if (arch_split_virtual(dir.c_str(), virt_archive, sizeof(virt_archive),
                           virt_prefix, sizeof(virt_prefix))) {
        app_panes_cancel_op(app);
        return;
    }
    std::string vroot, vrpath;
    if (net_split_virtual(dir.c_str(), &vroot, &vrpath)) {
        // A folder or a rename on a server goes over the transport. An empty new
        // file is not offered: it would be an upload of nothing, and the editor
        // that follows a local new file would have nothing local to edit.
        const NetSite* site = remote_site_for_root(app, vroot);
        if (!site) { app_panes_cancel_op(app); return; }
        std::string err;
        if (app->op == OP_NEWFILE) {
            snprintf(app->op_error, sizeof(app->op_error),
                     "Empty new files are not created on a server. Use F5 to put a local file there.");
            return;
        }
        if (app->op == OP_MKDIR) {
            std::string rpath = vrpath.empty() ? app->op_name : vrpath + "/" + app->op_name;
            if (net_mkdir(*site, rpath, &err)) {
                snprintf(app->status_msg, sizeof(app->status_msg), "Created %s",
                         net_virtual_path(vroot, rpath).c_str());
                refresh_pane(&active, active.current_dir);
                app_panes_cancel_op(app);
            } else {
                // The name is still wrong or taken; let the user fix it in place.
                snprintf(app->op_error, sizeof(app->op_error), "Cannot create that folder: %s",
                         err.c_str());
            }
            return;
        }
        if (app->op == OP_RENAME) {
            if (app->op_plan.names.empty()) { app_panes_cancel_op(app); return; }
            std::string old_path = vrpath.empty() ? app->op_plan.names[0]
                                                  : vrpath + "/" + app->op_plan.names[0];
            if (net_rename(*site, old_path, app->op_name, &err)) {
                snprintf(app->status_msg, sizeof(app->status_msg), "Renamed to %s", app->op_name);
                refresh_pane(&active, active.current_dir);
                app_panes_cancel_op(app);
            } else {
                snprintf(app->op_error, sizeof(app->op_error), "Cannot rename to %s: %s",
                         app->op_name, err.c_str());
            }
            return;
        }
    }
    if (app->op == OP_MKDIR) {
        char path[4100];
        snprintf(path, sizeof(path), "%s/%s", dir.c_str(), app->op_name);
        if (fs_make_dir(path)) {
            snprintf(app->status_msg, sizeof(app->status_msg), "Created %s", path);
            refresh_pane(&active, active.current_dir);
        } else {
            snprintf(app->op_error, sizeof(app->op_error), "Cannot create that folder: %s",
                     strerror(errno));
            return;   // the name is still wrong or taken; let the user fix it in place
        }
    } else if (app->op == OP_NEWFILE) {
        char path[4100];
        snprintf(path, sizeof(path), "%s/%s", dir.c_str(), app->op_name);
        if (!fs_touch(path)) {
            snprintf(app->op_error, sizeof(app->op_error), "Cannot create %s: %s", path, strerror(errno));
            return;
        }
        snprintf(app->status_msg, sizeof(app->status_msg), "Created %s", path);
        refresh_pane(&active, active.current_dir);
        app_panes_cancel_op(app);
        spawn_terminal_editor(app, path);
        return;
    } else if (app->op == OP_RENAME) {
        // The entry named on the dialog's "Renaming:" line, and nothing else:
        // several entries cannot take one name.
        if (app->op_plan.names.empty()) {
            app_panes_cancel_op(app);
            return;
        }
        char src[4100], dst[4100];
        snprintf(src, sizeof(src), "%s/%s", dir.c_str(), app->op_plan.names[0].c_str());
        snprintf(dst, sizeof(dst), "%s/%s", dir.c_str(), app->op_name);
        if (!fs_move_entry(src, dst)) {
            snprintf(app->op_error, sizeof(app->op_error), "Cannot rename to %s: %s",
                     app->op_name, strerror(errno));
            return;
        }
        snprintf(app->status_msg, sizeof(app->status_msg), "Renamed to %s", app->op_name);
        refresh_pane(&active, active.current_dir);
    }
    app_panes_cancel_op(app);
}

// Drop the operation and everything it captured. Nothing has been written at
// this point: the dialog is the only place copy, move and the name operations
// are agreed to.
void app_panes_cancel_op(AppPanes* app) {
    app->op = OP_NONE;
    app->op_name[0] = '\0';
    app->op_error[0] = '\0';
    app->op_run_bg = false;
    app->op_plan = OpPlan();
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
    if (archive_readonly(app, active)) return;
    if (remote_blocked(app, active, "Copying a file alongside itself")) return;
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
    if (archive_readonly(app, active) || archive_readonly(app, other)) return;
    if (remote_blocked(app, active, "Symlinks") || remote_blocked(app, other, "Symlinks")) return;
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
    if (in_remote(active)) {
        // A remote directory's size is the sum of a tree this machine does not
        // have. The listing's own sizes are shown; the total would be a download.
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Directory sizes need a local directory");
        return;
    }
    if (in_archive(active)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Directory sizes need a real directory");
        return;
    }
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
// Alt+F9 extract, Alt+Shift+F9 test. They are matched here so they never fall
// through to plain F5/F6/F9, which would quietly copy or move instead.
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
    // While the op dialog is up its field owns the text keys: arrows, Home, End
    // and Delete move the caret inside a long typed path instead of walking the
    // file list.
    if (app && app->op != OP_NONE) {
        switch (key) {
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
        case SDLK_HOME: case SDLK_END: case SDLK_DELETE: case SDLK_BACKSPACE:
            return false;
        default: break;
        }
    }
    // Same for the Options dialog's font filter: typing "dejavu" narrows a list
    // of hundreds of files, it does not scroll the pane behind the dialog.
    if (app && app->options_open && app->options_field_active) {
        switch (key) {
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
        case SDLK_HOME: case SDLK_END: case SDLK_DELETE: case SDLK_BACKSPACE:
            return false;
        default: break;
        }
    }
    // And for the remote dialogs. The site form and the quick-connect line are
    // text fields, so arrows, Home, End and Delete belong to the caret inside
    // them; without this the caret never moves and the pane behind the dialog
    // walks instead, which reads as the dialog ignoring what was typed.
    if (app && (app->remote_quick_open || (app->remote_mgr_open && g_net_form.open))) {
        switch (key) {
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
        case SDLK_HOME: case SDLK_END: case SDLK_DELETE: case SDLK_BACKSPACE:
            return false;
        default: break;
        }
    }
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

    // ---- the operation dialog (F5, F6, F7, Shift+F4, F2) ----
    // Enter runs what the dialog describes and Esc drops it; every other key is
    // either text for the field or nothing. Text keys never reach here -
    // app_key_is_special lets the editing keys through so the caret, selection
    // and clipboard in the field keep working - so a stray 'd' cannot delete a
    // file while a destination is being typed.
    if (app->op != OP_NONE) {
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            // Shift+Enter is the keyboard half of the dialog's "Run in
            // background" button. It is raised for any copy or move and honoured
            // only if the plan turns out to cross a network: the destination is
            // not resolved until commit_op() runs, so the key cannot know yet,
            // and run_op_plan() clears the request either way.
            if ((mod & SDL_KMOD_SHIFT) && (app->op == OP_COPY || app->op == OP_MOVE))
                app->op_run_bg = true;
            app_panes_commit_op(app);
        } else if (key == SDLK_ESCAPE) app_panes_cancel_op(app);
        return;
    }

    // ---- the remote dialogs (Ctrl+N quick connect, Ctrl+F site manager) ----
    // Enter and Esc are the only keys these take; every other key is text for a
    // field. In the manager, Enter commits the editor when one is showing, which
    // is what a keyboard user expects from a form, and Esc steps out of the form
    // before it closes the window.
    if (app->remote_quick_open) {
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) { remote_quick_commit(app); return; }
        if (key == SDLK_ESCAPE) {
            app->remote_quick_open = false;
            app->remote_quick[0] = '\0';
            return;
        }
        // Nothing else may reach the panes while a connect is being typed. Ctrl+N
        // is the one exception, because pressing it again is also the way out.
        if (!(key == SDLK_N && (mod & SDL_KMOD_CTRL))) return;
    }
    if (app->remote_mgr_open) {
        if (key == SDLK_ESCAPE) {
            if (g_net_form.open) { g_net_form.open = false; app->remote_edit = -1; }
            else app->remote_mgr_open = false;
            return;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            if (g_net_form.open) { remote_save_form(app); return; }
            std::vector<NetRow> rows = net_rows(app);
            int c = app->remote_cursor;
            if (c < 0 && app->remote_edit >= 0 &&
                app->remote_edit < (int)app->remote_sites.size())
                c = app->remote_edit;   // a row picked with the mouse before the cursor moved
            if (c >= 0 && c < (int)rows.size()) {
                const NetRow& row = rows[c];
                remote_open(app, row.adhoc ? app->remote_adhoc[row.idx]
                                           : app->remote_sites[row.idx]);
            }
            return;
        }
        // The arrows walk the list of connections and Enter opens what they land
        // on. While the editor is showing the arrows belong to the caret inside a
        // field, and app_key_is_special has already handed them to ImGui, so this
        // only ever runs over the bare list.
        if (!g_net_form.open) {
            int next = net_cursor_step(app->remote_cursor, (int)net_rows(app).size(), key);
            if (next != app->remote_cursor) {
                app->remote_cursor = next;
                g_net_cursor_moved = true;
                return;
            }
        }
        // The manager is a window over the panes, not a widget beside them. Until
        // this returned, Enter on a bare list activated whatever the pane cursor
        // happened to be on and the arrows walked a list that was out of sight.
        // Everything the dialog does not take itself is dropped; Ctrl+F is left
        // through because it is also the binding that closes the window.
        if (!(key == SDLK_F && (mod & SDL_KMOD_CTRL))) return;
    }

    // ---- full viewer keys ----
    if (app->viewer_open) {
        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER ||
            key == SDLK_Q) { close_viewer(app); return; }
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
    if (app->options_open && key == SDLK_ESCAPE) { app->options_open = false; return; }
    // ---- pack dialog (Alt+F5): Enter packs, Esc cancels, and no other key
    //      reaches the panes while it is up ----
    if (app->pack_open) {
        // Enter asks for the pack; the tool runs between frames, because it
        // blocks and pumps its own progress UI.
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) app->pending_op = DEFER_PACK;
        else if (key == SDLK_ESCAPE) { app->pack_open = false; app->pack_names.clear(); }
        return;
    }

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
        // TC's archive keys. Alt+F5 asks for the archive name in the pack
        // dialog; the rest run straight away with the progress dialog.
        if (key == SDLK_F5) do_pack(app, shift);
        else if (key == SDLK_F6) do_extract(app, true);
        else if (shift) do_archive_test(app);
        else do_extract(app, false);
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
                view_entry(app, active, active.selected);
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
    // The spec's FTP bindings. Ctrl+F opens the saved-site manager, Ctrl+N one
    // line of quick connect; both are dialogs, so nothing blocks here.
    if (key == SDLK_F && (mod & SDL_KMOD_CTRL)) {
        app->remote_mgr_open = !app->remote_mgr_open;
        app->remote_note[0] = '\0';
        return;
    }
    if (key == SDLK_N && (mod & SDL_KMOD_CTRL)) {
        app->remote_quick_open = !app->remote_quick_open;
        app->remote_focus_request = true;
        app->remote_note[0] = '\0';
        return;
    }

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
    // Invert lives on TC's Num*, which frees Ctrl+I for TC's pane switch below.
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
        if (key == SDLK_P) { cmd_put_path(app, active); return; }
    }

    // ---- file operations ----
    // Alt+F3: open the cursor entry with the desktop's default application.
    if (key == SDLK_F3 && (mod & SDL_KMOD_ALT)) { open_cursor_default(app); return; }
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
        rename_cursor(app);
        return;
    }
    // TC F7: a dialog, not a prompt in the bar, so the folder name is typed into
    // a field that already has the keyboard and Enter is the whole operation.
    if (key == SDLK_F7) {
        if (branch_blocked(app, active)) return;
        open_name_dialog(app, OP_MKDIR, nullptr);
        return;
    }
    // TC Shift+F4: create an empty file and open it in the editor.
    if (key == SDLK_F4 && shift) {
        if (branch_blocked(app, active)) return;
        open_name_dialog(app, OP_NEWFILE, nullptr);
        return;
    }
    if (key == SDLK_F4) {
        edit_cursor(app);
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
        rename_cursor(app);
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
    // TC Ctrl+I: switch to the target pane. It is Tab, not Ctrl+Tab, which
    // cycles this pane's tabs.
    if (key == SDLK_I && (mod & SDL_KMOD_CTRL)) { app->left_focus = !app->left_focus; return; }
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
