#include "panes.h"
#include "filesystem.h"
#include "archive.h"
#include "proc.h"   // the shared child-process runner: pipe, /dev/null stdin, cancel
#include "sfv.h"
#include "text.h"    // .sfv verification: CRC-32 and the parser
#include "terminal.h" // finding a terminal to run the editor in, and its argument shape
#include "platform.h" // home, settings, temp dir, and handing things to the desktop
#include "img.h"     // picture previews: what a file is, and its pixels
#include "texture.h" // and getting those pixels onto the screen
#include "icons.h"   // the small icon in front of each name
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
#ifdef _WIN32
// SDL_opengl.h brings in <windows.h>, and winbase.h's OpenFile flags include an
// OF_CANCEL that would turn the dialog's enumerator below into a number.
#undef OF_CANCEL
#endif
#include <string.h>
#include <strings.h>   // strncasecmp: quick search prefix match
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#ifndef _WIN32
#include <pwd.h>
#include <grp.h>
#endif
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <cerrno>
#include <libgen.h>
#include <float.h>
#include <time.h>
#include <algorithm>
#include <map>
#include <condition_variable>
#include <deque>

// Rows per Page Up / Page Down step (approx. visible rows in a 800px pane)
static const int PAGE_SIZE = 18;

// Which control in the operation dialog the keyboard is on. The ids are fixed
// rather than positions in a list: the dialog draws "Run in background" only for a
// copy or move that crosses a network, so the ring changes length, and an id that
// meant Cancel keeps meaning that whatever else moved.
enum OpFocus {
    OF_FIELD  = 0,   // the text field: a name, or the destination
    OF_RUN    = 1,   // the dialog's own verb: Copy, Move, Rename...
    OF_BG     = 2,   // "Run in background", when the dialog offers it
    OF_CANCEL = 3,
};

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
    op_focus = OF_FIELD;
    status_msg[0] = '\0';
    console_open = false;
    console_field_active = false;
    console_focus_request = false;
    console_input[0] = '\0';
    console_input_reload = false;
    console_hist_pos = 0;
    console_scroll_lock = false;
    confirm = CONF_NONE;
    confirm_text[0] = '\0';
    hotlist_open = false;
    dirhist_open = false;
    console_hist_open = false;
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
    pending_confirm_answer = CONF_ANS_NONE;
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

// What the rows on screen add up to, for the status row under each pane.
//
// It reads `view` and not `entries`, because a quick filter is the one thing
// about a pane the user changed on purpose: a total that ignored it would be a
// second, contradicting source of truth about a folder the user can already see.
//
// A directory contributes its count and nothing to the bytes. Its own st_size is
// an inode, not its content, and the recursive total is a tree walk a frame has
// no business starting - that is what Ctrl+L and Alt+Shift+Enter exist for, and
// this row is what tells a user they are worth running.
struct PaneTotals {
    long long bytes;      // file sizes over the visible rows
    int files, dirs;
    long long m_bytes;    // the same over the marked rows
    int m_files, m_dirs;
};

static PaneTotals pane_totals(const Pane& p) {
    PaneTotals t = {0, 0, 0, 0, 0, 0};
    for (size_t r = 0; r < p.view.size(); r++) {
        int i = p.view[r];
        if (i < 0 || i >= p.entry_count) continue;
        FileEntry* e = p.entries[i];
        // ".." is a way out of the folder, not a thing in it.
        if (!e || strcmp(e->name, "..") == 0) continue;
        bool dir    = is_directory(e->name, e->mode);
        bool marked = i < (int)p.marked.size() && p.marked[i];
        if (dir) { t.dirs++; if (marked) t.m_dirs++; continue; }
        t.files++;
        t.bytes += e->size;
        if (marked) { t.m_files++; t.m_bytes += e->size; }
    }
    return t;
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
static void remote_rows_to_entries(const std::vector<FileEntry>& rows,
                                   FileEntry*** out_entries, int* out_count) {
    *out_entries = nullptr;
    *out_count = 0;

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

// ---------------------------------------------------------------------------
// Listing a site in the background
// ---------------------------------------------------------------------------
//
// A listing used to run inside refresh_pane(), on the UI thread, and a server
// that is slow to answer - or a host that does not answer at all, which curl
// waits out through a connect timeout, a retry and a second command - froze the
// whole window for as long as that took. So a remote directory is now asked for
// here and arrives later: refresh_pane() posts a request and puts a ".." row in
// the pane so it can be walked out of, and browse_reap() installs the rows once
// a frame when they come back.
//
// One worker thread does every listing. libcurl's handle cache is per thread
// (remote.cpp), so one long-lived thread is what keeps the login reused from one
// directory to the next, exactly as when the UI thread did it. That also means a
// forget - a disconnect, an edited site - has to reach that thread's cache, so
// it travels down the same queue.
//
// A request is wanted for as long as it is the newest one its pane made. A pane
// that navigates elsewhere, or is cancelled with Esc, stops wanting it, and the
// listing's progress callback - which curl calls during the connect and the login
// as well - turns that into an abort within about a second.

struct BrowseReq {
    uint64_t    id;
    const Pane* pane;
    NetSite     site;
    std::string dir;       // the pane's virtual path, which the result has to match
    std::string rpath;
};

struct BrowseDone {
    uint64_t               id = 0;
    const Pane*            pane = nullptr;
    std::string            dir;
    bool                   ok = false;
    std::vector<FileEntry> rows;
    std::string            err;
};

struct BrowseState {
    std::mutex                      mu;
    std::condition_variable         cv;
    std::thread                     worker;
    bool                            stop = false;
    std::deque<BrowseReq>           queue;
    std::vector<std::string>        forget;   // roots to drop on the worker; "" = every site
    std::vector<BrowseDone>         done;
    std::map<const Pane*, uint64_t> wanted;   // each pane's newest request
    uint64_t                        next_id = 0;
};

// Never destroyed: a std::thread still joinable in a static destructor ends the
// process, and a test that never frees its app must not do that on the way out.
static BrowseState& g_browse = *new BrowseState;

static bool browse_wanted_cb(long long, long long, int, int, void* user) {
    const BrowseReq* r = (const BrowseReq*)user;
    std::lock_guard<std::mutex> lk(g_browse.mu);
    if (g_browse.stop) return false;
    auto it = g_browse.wanted.find(r->pane);
    return it != g_browse.wanted.end() && it->second == r->id;
}

static void browse_main() {
    std::unique_lock<std::mutex> lk(g_browse.mu);
    for (;;) {
        g_browse.cv.wait(lk, [] {
            return g_browse.stop || !g_browse.queue.empty() || !g_browse.forget.empty();
        });
        if (g_browse.stop) break;
        if (!g_browse.forget.empty()) {
            std::vector<std::string> roots;
            roots.swap(g_browse.forget);
            lk.unlock();
            for (const std::string& r : roots) {
                if (r.empty()) net_forget(nullptr);
                else net_forget_root(r.c_str());
            }
            lk.lock();
            continue;
        }
        BrowseReq r = std::move(g_browse.queue.front());
        g_browse.queue.pop_front();
        auto it = g_browse.wanted.find(r.pane);
        if (it == g_browse.wanted.end() || it->second != r.id) continue;   // moved on already
        lk.unlock();

        BrowseDone d;
        d.id = r.id;
        d.pane = r.pane;
        d.dir = r.dir;
        d.ok = net_list(r.site, r.rpath, &d.rows, &d.err, browse_wanted_cb, &r);

        lk.lock();
        g_browse.done.push_back(std::move(d));
    }
}

// Ask for a listing on behalf of `pane`. Anything the pane asked for before and
// has not received is no longer wanted.
static uint64_t browse_request(const Pane* pane, const NetSite& site, const std::string& dir,
                               const std::string& rpath) {
    std::lock_guard<std::mutex> lk(g_browse.mu);
    if (!g_browse.worker.joinable()) {
        g_browse.stop = false;
        g_browse.worker = std::thread(browse_main);
    }
    uint64_t id = ++g_browse.next_id;
    g_browse.wanted[pane] = id;
    for (size_t i = 0; i < g_browse.queue.size();) {
        if (g_browse.queue[i].pane == pane) g_browse.queue.erase(g_browse.queue.begin() + (long)i);
        else i++;
    }
    g_browse.queue.push_back({ id, pane, site, dir, rpath });
    g_browse.cv.notify_one();
    return id;
}

// The pane no longer wants what it asked for. A listing in progress stops at its
// next progress call.
static void browse_cancel(const Pane* pane) {
    std::lock_guard<std::mutex> lk(g_browse.mu);
    g_browse.wanted.erase(pane);
}

// Drop the worker's cached connection for a site root, or every one for "".
static void browse_forget(const std::string& root) {
    std::lock_guard<std::mutex> lk(g_browse.mu);
    if (!g_browse.worker.joinable()) return;   // never listed anything: nothing cached
    g_browse.forget.push_back(root);
    g_browse.cv.notify_one();
}

static void browse_take(std::vector<BrowseDone>* out) {
    std::lock_guard<std::mutex> lk(g_browse.mu);
    out->swap(g_browse.done);
    g_browse.done.clear();
}

// Stop the worker and wait for it. Its connections close with its thread, which
// is why this has to run before net_shutdown().
static void browse_stop() {
    {
        std::lock_guard<std::mutex> lk(g_browse.mu);
        g_browse.stop = true;
        g_browse.wanted.clear();
        g_browse.queue.clear();
        g_browse.cv.notify_one();
    }
    if (g_browse.worker.joinable()) g_browse.worker.join();
    std::lock_guard<std::mutex> lk(g_browse.mu);
    g_browse.done.clear();
    g_browse.forget.clear();
    g_browse.stop = false;
}

// Close a site's connections on every thread that browses: the UI thread's own
// (the mutations - mkdir, rename, chmod - still run there) and the worker's.
static void remote_forget(const NetSite* site) {
    net_forget(site);
    browse_forget(site ? net_site_root(*site) : std::string());
}

static void remote_forget_root(const char* root) {
    net_forget_root(root);
    if (root && *root) browse_forget(root);
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

// ---------------------------------------------------------------------------
// Network shares (SMB). On Windows a share is a path - \\server\share - that the
// file system opens like any other, and "\\server" on its own lists the server's
// shares (src/win/posix.cpp). On Linux the same places are smb:// URLs, which the
// kernel does not open; they go through GVfs, the layer the desktop's own file
// managers use: `gio list` names a server's shares, and `gio mount` mounts one at
// a local path under /run/user, which the pane then lists like any directory.
// ---------------------------------------------------------------------------

static bool smb_is_url(const char* s) { return strncasecmp(s, "smb://", 6) == 0; }

// A line that names a place on the network and nothing else: \\server,
// \\server\share, smb://server/share, and on Windows //server too. Typed into the
// command line it means "go there", which is how Explorer's address bar and TC's
// command line read it; no shell has a command by that name.
static bool names_network_place(const char* s) {
    if (s[0] == '\\' && s[1] == '\\' && s[2] && s[2] != '\\') return true;
#ifdef _WIN32
    if (s[0] == '/' && s[1] == '/' && s[2] && s[2] != '/') return true;
#endif
    return smb_is_url(s);
}

// \\server\share\dir in the URL form Linux needs: smb://server/share/dir.
static std::string smb_url_from_unc(const char* s) {
    std::string r = "smb://";
    for (const char* p = s + 2; *p; p++) r += (*p == '\\') ? '/' : *p;
    return r;
}

#ifndef _WIN32
// Run gio and collect what it writes. A mount can wait on a server that does not
// answer, so every read has a limit, and running out of it is a failure.
static bool gio_run(const std::vector<std::string>& argv, std::string* out) {
    Proc p;
    std::string err;
    if (!proc_start(argv, nullptr, kNoEnv, &p, &err)) { *out = err; return false; }
    char buf[4096];
    for (;;) {
        long n = proc_read(&p, buf, sizeof buf, 30000);
        if (n > 0) { out->append(buf, (size_t)n); continue; }
        if (n == -1) {
            proc_finish(&p, true);
            *out = "no answer in 30 seconds";
            return false;
        }
        break;
    }
    int code = proc_finish(&p, false);
    if (code == 127) *out = "gio is not installed";
    return code == 0;
}

// The first line of a tool's complaint, which is the one that says what went wrong.
static std::string first_line(const std::string& s) {
    size_t nl = s.find('\n');
    std::string r = s.substr(0, nl);
    while (!r.empty() && (r.back() == '\r' || r.back() == ' ')) r.pop_back();
    return r;
}

// Where an smb:// URL is on this machine. A URL with a share comes back as the
// local path GVfs mounted it at (`local`); a bare server comes back as its shares.
// False with the reason in `err`, which names what to install or do.
static bool smb_resolve(const std::string& url, std::string* local,
                        std::vector<std::string>* shares, std::string* err) {
    std::string rest = url.substr(6);
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    size_t slash = rest.find('/');
    std::string server = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "" : rest.substr(slash + 1);
    if (server.empty()) { *err = "smb:// needs a server: smb://server/share"; return false; }
    const char* hint = " - smb:// goes through GVfs: install gvfs and gvfs-smb, or mount the "
                       "share with mount.cifs and open the mount point";
    // gio takes a URI, so a space in a share or a folder name is %20 there.
    std::string uri = "smb://" + server + "/" + net_url_escape(path, true);

    if (path.empty()) {
        std::string out;
        if (!gio_run({ "gio", "list", uri }, &out)) {
            *err = url + ": " + first_line(out) + hint;
            return false;
        }
        size_t at = 0;
        while (at < out.size()) {
            size_t nl = out.find('\n', at);
            std::string name = out.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? out.size() : nl + 1;
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            if (name.empty() || name.back() == '$') continue;   // administrative shares
            shares->push_back(name);
        }
        return true;
    }

    // Mount the share (an already mounted one says so and is not an error), then ask
    // where its path is on this machine.
    size_t share_end = path.find('/');
    std::string share_uri = "smb://" + server + "/" +
                            net_url_escape(path.substr(0, share_end), true);
    std::string mount_out, info_out;
    gio_run({ "gio", "mount", share_uri }, &mount_out);
    if (gio_run({ "gio", "info", uri }, &info_out)) {
        size_t at = info_out.find("local path: ");
        if (at != std::string::npos) {
            *local = first_line(info_out.substr(at + 12));
            if (!local->empty()) return true;
        }
    }
    std::string why = first_line(mount_out.empty() ? info_out : mount_out);
    if (why.empty()) why = "not mounted";
    *err = url + ": " + why +
           (why.find("assword") != std::string::npos
                ? " - a share with a password: mount it once in the file manager, or run "
                  "`gio mount " + share_uri + "` in a terminal"
                : std::string(hint));
    return false;
}
#endif

// The directory above a pane's, for "..", Backspace and Ctrl+PgUp. dirname() is the
// answer everywhere except above an smb:// server on Linux, which is a URL with
// nothing above it: that goes home.
static void pane_parent(const char* dir, char* out, size_t n) {
#ifndef _WIN32
    if (smb_is_url(dir)) {
        snprintf(out, n, "%s", plat_home_dir().c_str());
        return;
    }
#endif
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", dir);
    snprintf(out, n, "%s", dirname(buf));
}

// Collapse a local path to one spelling: "//home/adam//x/" becomes "/home/adam/x".
// Worth doing rather than keeping the bytes the caller typed, for two reasons.
//
// A doubled slash turns one directory into two different strings. `strcmp(path,
// current_dir)` then says they are two places, so a self-refresh counts as a
// navigation and drops the filter; the directory history fills with near-duplicates;
// and a tab stops matching the pane it belongs to.
//
// And it never repairs itself: glibc's dirname() keeps a leading "//" (`dirname
//("//home/adam//github")` is `//home/adam`) and adds a trailing one, so a doubled
// slash that gets in through a join spreads into the tabs, the history and the config
// file. That is how a saved config ended up reading
// `left_tab0 = //home/adam/github/fugger_2_reloaded/build`.
//
// A site's virtual path is left byte for byte: its separators belong to a URL and to
// a path on a server, and rewriting them changes which file is meant. An archive's
// virtual path is collapsed like any other local path - the doubled slash always comes
// from the filesystem half, and a member name a listing can build never contains one.
//
// On Windows the one spelling is "C:/Users/adam": a backslash is the same separator
// as a slash and becomes one, the drive letter is upper case, and a drive's root keeps
// its slash - "C:" alone names the drive's current directory, which is somewhere else.
// A path with no drive ("/Users") is on the system drive. A UNC path keeps the two
// slashes that make it one.
static void normalize_pane_path(const char* in, char* out, size_t n) {
    if (!in || !*in) { snprintf(out, n, "%s", plat_root_of(nullptr).c_str()); return; }
    if (net_is_remote(in)) { snprintf(out, n, "%s", in); return; }
#ifndef _WIN32
    // An SMB URL is the network's spelling, resolved to a mount when it is listed
    // (see smb_resolve); collapsing its "//" would make it a local path.
    if (smb_is_url(in)) { snprintf(out, n, "%s", in); return; }
#endif

    std::string s(in), r;
    size_t keep = 1;   // the shortest the path may get: the root it starts with
#ifdef _WIN32
    // smb://server/share is \\server\share by another name.
    if (smb_is_url(in)) s = "//" + s.substr(6);
    for (char& c : s) if (c == '\\') c = '/';
    if (s.size() >= 2 && s[1] == ':' && isalpha((unsigned char)s[0])) {
        s[0] = (char)toupper((unsigned char)s[0]);
        if (s.size() == 2 || s[2] != '/') s.insert(2, "/");
        keep = 3;
    } else if (s.size() >= 2 && s[0] == '/' && s[1] == '/') {
        r = "//";      // UNC: both slashes are the root, and the collapse starts after them
        s.erase(0, 2);
        keep = 2;
    } else if (s[0] == '/') {
        s = plat_root_of(nullptr).substr(0, 2) + s;
        keep = 3;
    }
#endif
    r.reserve(r.size() + s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '/') { r += s[i]; continue; }
        if (!r.empty() && r.back() == '/') continue;   // a run of slashes is one separator
        r += '/';
    }
    while (r.size() > keep && r.back() == '/') r.pop_back();   // a trailing one says nothing
    if (r.empty()) r = "/";                                    // "//" is still the root
    snprintf(out, n, "%s", r.c_str());
}

void refresh_pane(Pane* pane, const char* path) {
    // Every path a pane holds comes through this one door, which is the only place
    // the spelling has to be fixed once. `path` is often `pane->current_dir` itself,
    // so the result goes into its own buffer rather than being written back.
    char norm[4096];
    normalize_pane_path(path, norm, sizeof(norm));

    bool dir_changed = strcmp(pane->current_dir, norm) != 0;
    char previous[4096];
    snprintf(previous, sizeof(previous), "%s", pane->current_dir);
    char arch[4096], prefix[1024];
    std::string nroot, nrpath;

    // A reread of the site directory already on screen keeps its rows until the
    // new ones arrive: a pane that blanks for a second after every upload is
    // worse than one that is a second out of date. The marks go now, though - they
    // name files the operation that asked for the reread may have removed.
    if (!dir_changed && pane->entry_count > 0 && net_split_virtual(norm, &nroot, &nrpath)) {
        if (const NetSite* site = net_site_for(nroot)) {
            pane->remote_req = browse_request(pane, *site, norm, nrpath);
            pane->remote_req_ms = SDL_GetTicks();
            pane->marked.assign(pane->entry_count, false);
            pane->mark_undo.clear();
            return;
        }
    }
    // Anything else this pane was waiting for from a site is not wanted any more.
    if (pane->remote_req) {
        browse_cancel(pane);
        pane->remote_req = 0;
    }
    free_entries(pane->entries, pane->entry_count);
    // Nothing is a listing until one of the three branches below puts one there.
    // A pane that walks onto a site with no saved login gets no listing at all, and
    // rows that have been freed are only safe to leave behind with a count of zero to
    // reach them.
    pane->entries = nullptr;
    pane->entry_count = 0;
    strncpy(pane->current_dir, norm, sizeof(pane->current_dir) - 1);
    pane->current_dir[sizeof(pane->current_dir) - 1] = '\0';
    pane->selected = 0;
    pane->anchor = 0;
    pane->rel.clear();
    bool smb_listed = false;   // the listing is a server's shares, or a failure to reach it
#ifndef _WIN32
    if (smb_is_url(norm)) {
        std::string local, err;
        std::vector<std::string> shares;
        if (!smb_resolve(norm, &local, &shares, &err)) {
            g_net_note = err;
            smb_listed = true;     // nothing to show; the status line says why
        } else if (!local.empty()) {
            // A share: from here on the pane is in the directory GVfs mounted it at.
            normalize_pane_path(local.c_str(), norm, sizeof(norm));
            snprintf(pane->current_dir, sizeof(pane->current_dir), "%s", norm);
        } else {
            pane->entries = (FileEntry**)calloc(shares.size() + 1, sizeof(FileEntry*));
            if (pane->entries) {
                int i = 0;
                pane->entries[i++] = file_entry_new("..", S_IFDIR | 0755, 0, 0);
                for (const std::string& name : shares)
                    pane->entries[i++] = file_entry_new(name.c_str(), S_IFDIR | 0755, 0, 0);
                pane->entry_count = i;
            }
            smb_listed = true;
        }
    }
#endif
    if (smb_listed) {
        pane->archive_path[0] = '\0';
        pane->archive_prefix[0] = '\0';
        pane->remote_root[0] = '\0';
        pane->remote_path[0] = '\0';
        pane->remote_came_from[0] = '\0';
        pane->list_mode = LIST_NORMAL;
    } else if (net_split_virtual(norm, &nroot, &nrpath)) {
        const NetSite* site = net_site_for(nroot);
        std::string err;
        if (site) {
            // Only ".." for now, so the pane can be left while it waits; the
            // rows come from the worker (see browse_reap).
            pane->entries = (FileEntry**)calloc(1, sizeof(FileEntry*));
            if (pane->entries) {
                pane->entries[0] = file_entry_new("..", S_IFDIR | 0755, 0, 0);
                pane->entry_count = 1;
            }
            pane->remote_req = browse_request(pane, *site, norm, nrpath);
            pane->remote_req_ms = SDL_GetTicks();
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
    } else if (arch_split_virtual(norm, arch, sizeof arch, prefix, sizeof prefix)) {
        list_archive_dir(arch, prefix, &pane->entries, &pane->entry_count, &pane->rel);
        snprintf(pane->archive_path, sizeof pane->archive_path, "%s", arch);
        snprintf(pane->archive_prefix, sizeof pane->archive_prefix, "%s", prefix);
        pane->remote_root[0] = '\0';
        pane->remote_path[0] = '\0';
        pane->list_mode = LIST_ARCHIVE;
    } else {
        list_directory(norm, &pane->entries, &pane->entry_count);
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
    // Normalized before it goes into the history as well as into the listing: a
    // history full of `//home/adam` variants is the same bug one step later.
    char norm[4096];
    normalize_pane_path(path, norm, sizeof(norm));
    if (strcmp(norm, pane->current_dir) != 0) remember_selection(pane);
    push_history(pane, norm);
    refresh_pane(pane, norm);
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
    snprintf(path, sizeof(path), "%s/hotlist", plat_config_dir().c_str());
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

static void hotlist_save(AppPanes* app);

// Add a folder to the hotlist - the favorites the Alt+F1/F2 picker lists under the
// drives - unless it is there already. Returns false for a duplicate.
static bool hotlist_add(AppPanes* app, const char* dir) {
    for (const std::string& s : app->hotlist) if (s == dir) return false;
    app->hotlist.push_back(dir);
    hotlist_save(app);
    return true;
}

static void hotlist_save(AppPanes* app) {
    plat_make_config_dir();
    FILE* f = fopen(hotlist_file(), "w");
    if (!f) return;
    for (const std::string& s : app->hotlist) fprintf(f, "%s\n", s.c_str());
    fclose(f);
}

// ---------------------------------------------------------------------------
// Launch bar buttons, persisted one path per line under <settings>/launchbar
// ---------------------------------------------------------------------------

static std::string launchbar_file() {
    return plat_config_dir() + "/launchbar";
}

static AppPanes::LaunchItem launch_item_for(const std::string& path) {
    AppPanes::LaunchItem it;
    it.path = path;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        it.is_dir = S_ISDIR(st.st_mode);
        size_t slash = path.find_last_of('/');
        it.is_exec = !it.is_dir &&
                     is_executable(path.c_str() + (slash == std::string::npos ? 0 : slash + 1), st.st_mode);
    }
    return it;
}

static void launchbar_load(AppPanes* app) {
    app->launch_items.clear();
    FILE* f = fopen(launchbar_file().c_str(), "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n) app->launch_items.push_back(launch_item_for(line));
    }
    fclose(f);
}

static void launchbar_save(AppPanes* app) {
    plat_make_config_dir();
    FILE* f = fopen(launchbar_file().c_str(), "w");
    if (!f) return;
    for (const AppPanes::LaunchItem& it : app->launch_items) fprintf(f, "%s\n", it.path.c_str());
    fclose(f);
}

// The name a button goes by: the file name without the extension that only says
// it is a program or a shortcut.
static std::string launch_label(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos || slash + 1 == path.size())
                           ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        std::string ext = base.substr(dot + 1);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == "exe" || ext == "lnk" || ext == "bat" || ext == "cmd" ||
            ext == "desktop" || ext == "appref-ms" || ext == "url")
            base.erase(dot);
    }
    return base;
}

// Add a button at `at` (the end when out of range). A path that already has one
// is not added twice. Paths are kept the way the panes write them: forward
// slashes, no slash at the end.
static bool launchbar_add(AppPanes* app, const char* path, int at = -1) {
    std::string p = path;
    for (char& c : p) if (c == '\\') c = '/';
    while (p.size() > 1 && p.back() == '/' && !(p.size() == 3 && p[1] == ':')) p.pop_back();
    if (p.empty()) return false;
    for (const AppPanes::LaunchItem& it : app->launch_items)
        if (it.path == p) {
            snprintf(app->status_msg, sizeof(app->status_msg), "%s is on the launch bar already",
                     launch_label(p).c_str());
            return false;
        }
    if (at < 0 || at > (int)app->launch_items.size()) at = (int)app->launch_items.size();
    app->launch_items.insert(app->launch_items.begin() + at, launch_item_for(p));
    launchbar_save(app);
    snprintf(app->status_msg, sizeof(app->status_msg), "Added %s to the launch bar",
             launch_label(p).c_str());
    return true;
}

static void launchbar_remove(AppPanes* app, int i) {
    if (i < 0 || i >= (int)app->launch_items.size()) return;
    app->launch_items.erase(app->launch_items.begin() + i);
    launchbar_save(app);
}

// Move button `from` so that it lands in front of the one now at `to` (the end
// when `to` is the count).
static void launchbar_move(AppPanes* app, int from, int to) {
    int n = (int)app->launch_items.size();
    if (from < 0 || from >= n || to < 0 || to > n || to == from || to == from + 1) return;
    AppPanes::LaunchItem it = app->launch_items[from];
    app->launch_items.erase(app->launch_items.begin() + from);
    if (to > from) to--;
    app->launch_items.insert(app->launch_items.begin() + to, it);
    launchbar_save(app);
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
        if (!show_hidden && fs_entry_hidden(de)) continue;
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

// The same numbers without the four-column padding, which is there to right-align
// the size column. In a sentence that padding reads as a hole in the middle of it:
// "3 files  1 dirs    18" is the same fact written worse.
static void human_size_trim(off_t s, char* out, size_t n) {
    human_size(s, out, n);
    char* p = out;
    while (*p == ' ') p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
}

static void fmt_date(time_t t, char* out, size_t n) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, n, "%m-%d %H:%M", &tmv);
}

// ---------------------------------------------------------------------------
// Console: the Ctrl+Enter window, its builtins, and the shell it runs
// ---------------------------------------------------------------------------

// Defined with the remote dialogs. "cd ftp://..." in the console and the Ctrl+N
// field both end up there, so a typed address means one thing however it was
// entered.
static void remote_open_url(AppPanes* app, const char* url);

// Defined with the transfer rows, but the progress dialog needs it too: an ETA in
// seconds is a number the user has to do arithmetic on. See `fmt_secs`.
static void fmt_secs(double s, char* out, size_t n);

// Drop a multi-byte character a cut left half-written. A trailing run of
// continuation bytes is only a character if the lead byte in front of it promises
// exactly that many of them; if it promises more, the whole run is dropped rather
// than left for ImGui's decoder to guess at.
static void trim_partial_utf8(std::string& s) {
    size_t i = s.size();
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0 || i == s.size()) return;              // nothing half-written
    unsigned char lead = (unsigned char)s[i - 1];
    if (lead < 0xC0) return;                          // plain ASCII before the run
    int total = lead >= 0xF0 ? 4 : (lead >= 0xE0 ? 3 : 2);
    if ((int)(s.size() - i + 1) == total) return;     // complete: keep it
    s.resize(i - 1);
}

// A command's output is drawn as one block of text, so it has to be text by the
// time it lands. A progress bar that redraws one line with '\r' is otherwise a
// wall of control characters, `cat` of a binary file is not text at all, and
// ImGui's vertex builder is the wrong place to discover either. Cleaned on append
// rather than on draw, so nothing downstream has to remember to do it.
static void console_append(ConsoleJob* job, const char* s, size_t n) {
    std::lock_guard<std::mutex> lk(job->mu);
    if (job->out.size() >= MC_CONSOLE_MAX_OUTPUT) return;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\t') { job->out.push_back((char)c); continue; }
        if (c == '\r') continue;                      // a line redraw, not a line break
        if (c >= 0x20 && c != 0x7F) { job->out.push_back((char)c); continue; }
        job->out += "\xC2\xB7";                       // the rest becomes a middot
    }
    // >= and not >: a chunk that lands exactly on the cap would otherwise stop the
    // command's output short without ever saying that it did.
    if (job->out.size() >= MC_CONSOLE_MAX_OUTPUT) {
        job->out.resize(MC_CONSOLE_MAX_OUTPUT);
        trim_partial_utf8(job->out);
        job->out += "\n... output truncated ...";
    }
}

// The backlog is oldest first and bounded: dropping the oldest command is what
// makes a long session's transcript the same size as a short one.
static void console_push(AppPanes* app, const std::string& cwd, const std::string& cmd,
                         const std::string& out, int code, bool builtin) {
    ConsoleEntry e;
    e.cwd = cwd; e.cmd = cmd; e.out = out; e.code = code; e.builtin = builtin;
    app->console_log.push_back(e);
    if ((int)app->console_log.size() > MC_CONSOLE_MAX_ENTRIES)
        app->console_log.erase(app->console_log.begin(),
                               app->console_log.end() - MC_CONSOLE_MAX_ENTRIES);
}

// Commands for Up/Down recall, separate from the transcript because the transcript
// is a record of what happened and this is a list of things to type again. Two of
// the same command in a row is one entry.
static void console_hist_push(AppPanes* app, const std::string& line) {
    if (!app->console_hist.empty() && app->console_hist.back() == line) {
        app->console_hist_pos = (int)app->console_hist.size();
        return;
    }
    app->console_hist.push_back(line);
    if ((int)app->console_hist.size() > MC_CONSOLE_MAX_HIST)
        app->console_hist.erase(app->console_hist.begin(),
                                app->console_hist.end() - MC_CONSOLE_MAX_HIST);
    app->console_hist_pos = (int)app->console_hist.size();
}

// Write the console's line from outside the text field. From the moment a field has
// the keyboard ImGui edits its own copy of the buffer and displays that, so clearing
// or prefilling the buffer from the app is not enough: the field still shows the old
// text and writes it back over the app's change on the next frame. The reload request
// is what tells the field to take the buffer again, with the caret at the end.
static void console_set_input(AppPanes* app, const char* text) {
    snprintf(app->console_input, sizeof(app->console_input), "%s", text ? text : "");
    app->console_input_reload = true;
}

// Up/Down inside the console field walk the commands, which is what Up/Down mean
// in every shell. Walking past the newest one lands on a fresh empty line, so the
// gesture round-trips: Up Up Up Down Down Down leaves the field as it was.
static void console_history_step(AppPanes* app, bool up) {
    int n = (int)app->console_hist.size();
    if (n == 0) return;
    if (app->console_hist_pos > n) app->console_hist_pos = n;
    if (up) { if (app->console_hist_pos > 0) app->console_hist_pos--; }
    else    { if (app->console_hist_pos < n) app->console_hist_pos++; }
    if (app->console_hist_pos >= n) console_set_input(app, "");
    else console_set_input(app, app->console_hist[app->console_hist_pos].c_str());
}

// Settle a command that has finished: move the worker's output into the backlog
// entry and join the thread. Only a job that has published `done` is ever joined,
// and the join happens before anything can drop the record - the rule BgTransfer
// and QvFetch follow, for the same reason: a std::thread that is still joinable
// when it is destroyed ends the process.
static void console_reap(AppPanes* app) {
    ConsoleJob* job = app->console_job.get();
    if (!job || !job->done.load()) return;
    std::string out;
    { std::lock_guard<std::mutex> lk(job->mu); out = job->out; }
    int code = job->code;
    bool cancelled = job->cancel.load();
    if (job->worker.joinable()) job->worker.join();
    app->console_job.reset();
    if (app->console_log.empty()) return;
    ConsoleEntry& e = app->console_log.back();   // the running command is always the last
    e.out = out;
    e.code = code;
    e.cancelled = cancelled;
    // Silence on success. A transcript that says "exit 0" after every command is
    // noise, and the exit code is only interesting when it is not zero.
    if (cancelled) {
        if (!e.out.empty()) e.out += "\n";
        e.out += "interrupted";
    } else if (code != 0) {
        if (!e.out.empty()) e.out += "\n";
        e.out += "exit " + std::to_string(code);
    }
    app->console_scroll_lock = false;
}

// The worker: run the line through the shell in the directory the focused pane is
// showing, and feed the output to the window as it arrives, which is what lets a
// `find` that takes a minute show its results while it is still running. A read
// with a timeout rather than a blocking one, because a cancel has to land while
// the tool is silent, not only when it next writes something.
static void console_worker(ConsoleJob* job, std::string line, std::string cwd) {
    Proc p;
    std::string err;
    // The shell, not execvp on the first word: pipes, redirection and globbing are
    // what make a console worth typing into.
    std::vector<std::string> argv = proc_shell_argv(line);
    if (!proc_start(argv, cwd.c_str(), kNoEnv, &p, &err)) {
        // No pipe or no fork. There is no child to report it, so the job does.
        console_append(job, err.c_str(), err.size());
        job->code = 127;
        job->done.store(true);
        return;
    }
    // Output goes to the window a line at a time, so a character is never split between
    // two reads when plat_console_text() decides what encoding it is in. Whatever is
    // waiting goes as soon as the command falls silent, which keeps a prompt that ends
    // without a newline on screen.
    std::string pending;
    auto flush = [&](size_t upto) {
        if (!upto) return;
        const std::string text = plat_console_text(pending.data(), upto);
        console_append(job, text.data(), text.size());
        pending.erase(0, upto);
    };
    char buf[4096];
    for (;;) {
        long n = proc_read(&p, buf, sizeof buf, 100);
        if (n == -1) {                            // silent: show what is waiting, check for a cancel
            flush(pending.size());
            if (job->cancel.load()) break;
            continue;
        }
        if (n <= 0) break;                        // EOF: the command is done
        pending.append(buf, (size_t)n);
        size_t nl = pending.find_last_of('\n');
        if (nl != std::string::npos) flush(nl + 1);
        else if (pending.size() > sizeof buf) flush(pending.size());
    }
    flush(pending.size());
    // code is written before done publishes it, which is the order the frame that
    // reads `done` relies on.
    job->code = proc_finish(&p, job->cancel.load());
    job->done.store(true);
}

// The console's `cd` moves a pane, not the process. The app's own working
// directory is not where the user is looking, and a console that changed it would
// move the ground under the next command without moving anything on screen. The
// path rules are the ones the command line had, unchanged: `~`, an absolute path,
// a path relative to the pane, and a URL, which is a directory too.
static void console_cd(AppPanes* app, const char* arg) {
    while (*arg == ' ') arg++;
    Pane& active = app->left_focus ? app->left : app->right;
    char target[4096];
    const std::string home = plat_home_dir();
    if (!*arg) {
        snprintf(target, sizeof(target), "%s",
                 !home.empty() ? home.c_str() : plat_root_of(active.current_dir).c_str());
    } else if (arg[0] == '~' && (arg[1] == '/' || arg[1] == '\\' || arg[1] == '\0')) {
        snprintf(target, sizeof(target), "%s%s", home.c_str(), arg + 1);
    } else if (net_is_remote(arg) || smb_is_url(arg)) {
        snprintf(target, sizeof(target), "%s", arg);
#ifndef _WIN32
    } else if (arg[0] == '\\' && arg[1] == '\\') {
        // \\server\share is what a Windows habit types; here it is an smb:// URL.
        snprintf(target, sizeof(target), "%s", smb_url_from_unc(arg).c_str());
#endif
    } else if (plat_path_is_absolute(arg)) {
        snprintf(target, sizeof(target), "%s", arg);
    } else {
        snprintf(target, sizeof(target), "%s/%s", active.current_dir, arg);
    }
    if (net_is_remote(target)) { remote_open_url(app, target); return; }
#ifndef _WIN32
    // An smb:// URL is not something opendir knows; listing it is what finds out
    // whether it can be reached, and the reason lands in the status line if not.
    if (smb_is_url(target)) { navigate_to(app, &active, target); return; }
#endif
    DIR* probe = opendir(target);
    if (probe) {
        closedir(probe);
        navigate_to(app, &active, target);
    } else {
        snprintf(app->status_msg, sizeof(app->status_msg), "cd: no such directory: %s", target);
    }
}

// Start a shell command in the focused pane's directory and record it in the
// backlog. Defined below console_run, which is the half that decides a line is a
// command at all; the other caller is Enter on an executable file, where the
// command is already decided.
static void console_start(AppPanes* app, const std::string& line);

// Run one console line. Builtins are answered here, on this thread; everything
// else is a shell command in the directory the focused pane is showing, on a
// worker, so a command that takes a minute does not take the UI with it.
static void console_run(AppPanes* app) {
    console_reap(app);
    if (app->console_job) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "A command is still running. Esc cancels it.");
        return;
    }

    char line[512];
    snprintf(line, sizeof(line), "%s", app->console_input);
    char* c = line;
    while (*c == ' ') c++;
    size_t len = strlen(c);
    while (len && c[len - 1] == ' ') c[--len] = '\0';   // trailing spaces are never meant
    if (!len) return;

    console_set_input(app, "");   // the line ran; the box is for the next one
    app->console_hist_open = false;
    console_hist_push(app, c);

    Pane& active = app->left_focus ? app->left : app->right;
    const std::string cwd = active.current_dir;

    if (strcmp(c, "exit") == 0 || strcmp(c, "quit") == 0) {
        console_push(app, cwd, c, "", -1, true);
        app->quit_request = true;
        return;
    }
    if (strcmp(c, "clear") == 0) {
        // Not an entry of its own: this is the command that removes entries, and
        // recording it would leave the transcript one line shorter than it looks.
        app->console_log.clear();
        app->console_scroll_lock = false;
        return;
    }
    if (strcmp(c, "pwd") == 0) { console_push(app, cwd, c, cwd, 0, true); return; }
    if (strcmp(c, "history") == 0) {
        std::string dump;
        for (size_t i = 0; i < app->console_hist.size(); i++) {
            dump += std::to_string(i + 1);
            dump += "  ";
            dump += app->console_hist[i];
            dump += '\n';
        }
        while (!dump.empty() && dump.back() == '\n') dump.pop_back();
        if (dump.empty()) dump = "(empty)";
        console_push(app, cwd, c, dump, 0, true);
        return;
    }
    // A line that is only a place on the network is a cd there, the way Explorer's
    // address bar takes \\server: cmd.exe would answer that it is not a command.
    bool place = names_network_place(c) || net_is_remote(c);
    if (place || strcmp(c, "cd") == 0 || strncmp(c, "cd ", 3) == 0) {
        console_cd(app, place ? c : c + 2);
        Pane& after = app->left_focus ? app->left : app->right;
        // The transcript says where the console is now sitting, because that is the
        // thing `cd` changed. If the pane did not move, the reason is what belongs
        // there instead.
        std::string out = after.current_dir;
        if (out == cwd && app->status_msg[0]) out = app->status_msg;
        console_push(app, cwd, c, out, -1, true);
        return;
    }

    // A shell command needs a directory the operating system understands. Inside
    // an archive or on a site the pane's directory is a name only this program
    // understands, and running the command in the directory the pane walked in
    // from would be a guess about what the user meant.
    if (in_archive(active) || in_remote(active)) {
        console_push(app, cwd, c,
                     in_archive(active)
                         ? "This pane is inside an archive: there is no directory here to run a "
                           "command in. Extract it, or cd to a real directory."
                         : "This pane is on a site. The console runs commands on this machine; cd "
                           "to a real directory first.",
                     127, true);
        return;
    }

    console_start(app, c);
}

// Start a shell command in the focused pane's directory and record it in the
// backlog. Split out of console_run because Enter on an executable file is the
// same thing with the command already decided: the answer belongs in the console
// either way, and it belongs on a thread either way.
static void console_start(AppPanes* app, const std::string& line) {
    console_reap(app);
    if (app->console_job) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "A command is still running. Esc cancels it.");
        return;
    }
    Pane& active = app->left_focus ? app->left : app->right;
    console_push(app, active.current_dir, line, "", -1, false);
    app->console_scroll_lock = false;
    app->console_job = std::make_unique<ConsoleJob>();
    app->console_job->worker = std::thread(console_worker, app->console_job.get(),
                                           line, std::string(active.current_dir));
}

// Open the console and, when asked, take the keyboard for its field. Focus is
// requested for the next frame rather than this one: a request lands on the frame
// after the ask, and asking while a press is in flight takes ActiveId away from
// whatever is being clicked, so the release finds a different item and the click
// is dropped.
static void console_show(AppPanes* app, bool focus) {
    app->console_open = true;
    if (focus) app->console_focus_request = true;
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

// "immc 0.9.0 (mc-clone)", or "immc 0.9.0" where the file is called immc too
// (Windows). The About window, --version and the title bar start from here, so
// they cannot drift apart.
void app_identity_line(char* out, size_t n) {
    if (!strcmp(MC_APP_NAME, MC_APP_BINARY))
        snprintf(out, n, "%s %s", MC_APP_NAME, MC_VERSION);
    else
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
             "written by Peter Garba\n"
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
#ifdef _WIN32
static const char* const kFontDirs[] = {
    "C:/Windows/Fonts",
};
#else
static const char* const kFontDirs[] = {
    "/usr/share/fonts", "/usr/local/share/fonts",
};
#endif

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
#ifdef _WIN32
    // Fonts installed for one user, which Windows 10 keeps out of the system folder.
    const std::string home = plat_home_dir();
    if (!home.empty()) dirs.push_back(home + "/AppData/Local/Microsoft/Windows/Fonts");
#else
    const char* home = getenv("HOME");
    if (home && *home) {
        dirs.push_back(std::string(home) + "/.local/share/fonts");
        dirs.push_back(std::string(home) + "/.fonts");
    }
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) dirs.push_back(std::string(xdg) + "/fonts");
#endif

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

[[maybe_unused]] static unsigned int mc_freetype_loader_flags(const char* name) {
    int i = mc_hinting_index(name);
    if (i < 0) i = 0;                       // unknown or empty: the loader's own choice
    unsigned int bits = (unsigned int)kHintingModes[i].bits;
#ifdef MC_HAVE_FREETYPE
    unsigned int flags = 0;
    if (bits & MC_HINT_NONE)      flags |= ImGuiFreeTypeLoaderFlags_NoHinting;
    if (bits & MC_HINT_LIGHT)     flags |= ImGuiFreeTypeLoaderFlags_LightHinting;
    if (bits & MC_HINT_STRONG)    flags |= ImGuiFreeTypeLoaderFlags_MonoHinting;
    if (bits & MC_HINT_NO_SMOOTH) flags |= ImGuiFreeTypeLoaderFlags_Monochrome;
    return flags;
#else
    // No loader to translate them for; the app's own bits keep the modes apart.
    return bits;
#endif
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

// The progress dialog's keyboard ring: Abort, and "Run in background" when the
// dialog is watching a transfer on its own thread. NONE is where a dialog with
// only Abort starts, so the first Enter after starting a copy does nothing.
enum { PROG_FOCUS_NONE = -1, PROG_FOCUS_ABORT = 0, PROG_FOCUS_BG = 1 };

static void op_focus_ring();

// Keys while the progress dialog is up. It runs inside its own event pump, not the
// app's key handler, so this is the whole of its keyboard: Tab, Shift+Tab and the
// arrows move between the buttons, Enter and Space press the one the ring is on.
// A held key's repeats are dropped by the caller.
static void prog_key(AppPanes* app, SDL_Keycode key, SDL_Keymod mod) {
    (void)mod;
    const bool two = app->prog_watch != nullptr;
    switch (key) {
    case SDLK_TAB: case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
        if (!two) app->prog_focus = PROG_FOCUS_ABORT;
        else app->prog_focus = app->prog_focus == PROG_FOCUS_BG ? PROG_FOCUS_ABORT : PROG_FOCUS_BG;
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
        if (app->prog_focus == PROG_FOCUS_BG && two) {
            app->prog_handoff = true;
        } else if (app->prog_focus == PROG_FOCUS_ABORT) {
            app->prog_abort = true;
            if (app->prog_watch) app->prog_watch->abort = true;
        }
        break;
    default:
        break;
    }
}

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

    char done_s[32], total_s[32], rem_s[32], rate_s[32], el_s[24], eta_s[24];
    human_size((off_t)app->prog_done, done_s, sizeof(done_s));
    human_size((off_t)app->prog_total, total_s, sizeof(total_s));
    // A transfer whose total is not known yet - a tree whose listing has not been
    // counted - has no remaining byte count, and `total - done` would print one as
    // a negative number of bytes. Unknown is the truth.
    if (app->prog_total > 0)
        human_size((off_t)(app->prog_total - app->prog_done), rem_s, sizeof(rem_s));
    else
        snprintf(rem_s, sizeof(rem_s), "unknown");

    double now = SDL_GetTicks() / 1000.0;
    double elapsed = now - app->prog_t0;
    double rate = elapsed > 0.05 ? (double)app->prog_done / elapsed : 0.0;
    human_size((off_t)rate, rate_s, sizeof(rate_s));

    ImGui::Text("Files:      %d / %d", app->prog_files_done, app->prog_files_total);
    ImGui::Text("%-10s %s of %s   Remaining: %s",
                app->prog_is_delete ? "Deleted:" : "Copied:", done_s, total_s, rem_s);
    // Minutes and hours, the way the transfer rows already spell them out. "ETA:
    // 3720s" is a number the user has to do arithmetic on; "1h02m" is not.
    fmt_secs(elapsed, el_s, sizeof(el_s));
    ImGui::Text("Throughput: %s/s   Elapsed: %s", rate_s, el_s);
    if (rate > 1.0 && app->prog_total > app->prog_done) {
        double eta = (double)(app->prog_total - app->prog_done) / rate;
        fmt_secs(eta, eta_s, sizeof(eta_s));
        ImGui::Text("ETA:        %s", eta_s);
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
    if (app->prog_focus == PROG_FOCUS_ABORT) op_focus_ring();
    // Only a transfer that is already on a thread of its own can be handed over,
    // and the handover asks nothing of the transfer: the row under the panes has
    // the numbers, the Abort button and the finish already. That is why this can
    // be pressed at any moment, in the middle of a file, and why it is not
    // offered for an operation that is running on this thread - there the dialog
    // is not watching the copy, it is the copy.
    if (app->prog_watch) {
        ImGui::SameLine();
        if (ImGui::Button("Run in background")) app->prog_handoff = true;
        if (app->prog_focus == PROG_FOCUS_BG) op_focus_ring();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The transfer goes on, and the list under the panes carries it.\n"
                              "Nothing about the transfer itself changes.");
    }
    if (app->prog_abort) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "stopping after the current file...");
    } else {
        ImGui::TextDisabled("Abort stops the operation. What is already transferred stays.");
        ImGui::TextDisabled(app->prog_watch ? "Tab or the arrows move between the buttons, Enter presses one."
                                            : "Tab puts the keyboard on Abort, Enter presses it.");
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
        // A held Enter from the dialog that started the copy repeats into this
        // one; only a fresh press counts.
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) prog_key(app, e.key.key, e.key.mod);
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
    app->prog_focus = PROG_FOCUS_NONE;
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
    app->prog_focus   = PROG_FOCUS_BG;   // Enter sends it to the background
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
#ifdef _WIN32
    // "C:/x.zip" is held in "C:/", not in "C:", which names the drive's current
    // directory rather than its root.
    if (out[0] && out[1] == ':' && !out[2]) snprintf(out + 2, cap - 2, "/");
#endif
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
    app->prog_focus = PROG_FOCUS_NONE;
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

    // Ask for the caret to land on the item submitted next. Claiming it keeps a
    // field that wants the keyboard on every frame from taking it back later in the
    // same frame.
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

// Does the dialog offer "Run in background" this frame? Asked of the plan here, in
// one place, so the drawing and the key handler cannot disagree about how long the
// ring is. The plan is not resolved until commit, so a local path typed over a
// remote source is still a network copy and still offers the button.
static bool op_dialog_network(const AppPanes* app) {
    if (app->op != OP_COPY && app->op != OP_MOVE) return false;
    return plan_crosses_network(app->op_plan, app->op_name);
}

// The ring: the field, then the buttons in the order the dialog draws them.
static void op_ring(const AppPanes* app, std::vector<int>* out) {
    out->clear();
    out->push_back(OF_FIELD);
    out->push_back(OF_RUN);
    if (op_dialog_network(app)) out->push_back(OF_BG);
    out->push_back(OF_CANCEL);
}

// Tab and Shift+Tab. The ring wraps through the field, because the field is where
// the destination is typed and a keyboard user leaving the last button wants to be
// back there, not parked on the first one. A ring the current id is no longer in -
// the destination was edited from a site to a local directory and the background
// button went away - restarts at the field instead of pointing at nothing.
static void op_tab(AppPanes* app, bool shift) {
    std::vector<int> ring;
    op_ring(app, &ring);
    int i = 0;
    while (i < (int)ring.size() && ring[i] != app->op_focus) i++;
    if (i >= (int)ring.size()) i = 0;
    int step = shift ? (int)ring.size() - 1 : 1;
    app->op_focus = ring[(i + step) % (int)ring.size()];
}

// The ring's mark. ImGui's keyboard navigation is switched off in this app - the
// panes own the arrow keys, and nav would take them - so nothing else says which
// button the keyboard is on. It gets the same outline the pane draws around its
// cursor row, which is the app's existing sign for "the keyboard is here".
static void op_focus_ring() {
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(ImVec2(lo.x - 3.0f, lo.y - 3.0f),
                                        ImVec2(hi.x + 3.0f, hi.y + 3.0f),
                                        mc_theme_colors()->cursor_outline, 0.0f, 0, 1.5f);
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
        // Clicking into the field is the keyboard going back to it, so the ring
        // follows rather than staying marked on a button the mouse just ignored.
        if (ImGui::IsItemClicked()) app->op_focus = OF_FIELD;
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
        if (ImGui::IsItemClicked()) app->op_focus = OF_FIELD;
    }

    if (app->op_error[0]) {
        ImGui::Spacing();
        text_wrap_colored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", app->op_error);
    }

    ImGui::Spacing();
    // The keyboard ring, drawn by hand: ImGui's own navigation is switched off in
    // this app because the panes own the arrow keys, so nothing else would say
    // which button the keyboard is on. It gets the same outline the pane draws
    // around its cursor row, the app's existing sign for "the keyboard is here".
    const bool network = op_dialog_network(app);
    // The button can vanish under a focus that is on it - the destination was
    // edited from a site to a local directory, and the ring loses a member. Step
    // back to the field instead of leaving the mark on a button that is not drawn.
    if (app->op_focus == OF_BG && !network) app->op_focus = OF_FIELD;
    // The button only records the request: copy and move block and pump their own
    // ImGui frames, which cannot happen while this dialog's frame is open.
    if (ImGui::Button(op_button_label(app->op))) app->pending_op = DEFER_OP_COMMIT;
    if (app->op_focus == OF_RUN) op_focus_ring();
    ImGui::SameLine();
    // The same operation, handed to a thread instead of run in front of the
    // panes. It commits exactly like the button above it and records the request
    // with the commit, because the choice has to survive the overwrite confirm
    // that may follow - which is why it lands in app->op_run_bg and not in a local.
    if (network) {
        if (ImGui::Button("Run in background")) {
            app->op_run_bg = true;
            app->pending_op = DEFER_OP_COMMIT;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The panes stay usable. The transfer is listed under them.");
        if (app->op_focus == OF_BG) op_focus_ring();
        ImGui::SameLine();
    }
    if (ImGui::Button("Cancel")) app_panes_cancel_op(app);
    if (app->op_focus == OF_CANCEL) op_focus_ring();
    ImGui::SameLine();
    text_wrap_disabled("Tab (then the arrows) moves between these, Enter runs what it is on, Esc cancels.%s",
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

// The name beside an existing one: "report.pdf" -> "report (2).pdf", then (3),
// until the name is free. The dot that counts is the last one after the last
// slash, so a directory - or a name with no dot at all - takes the number at the
// end, which is what a user expects from a folder called `src`.
//
// `taken` is what the destination directory already holds. When it is null the
// destination is a local directory and each candidate is asked directly: a stat
// per candidate is cheaper than listing the directory, and it cannot be stale.
// When it is not null it is a listing of a directory on a server, where asking
// per candidate is not something the protocol offers.
static std::string rename_beside(const std::string& dst, const std::set<std::string>* taken) {
    size_t slash = dst.find_last_of('/');
    std::string dir  = (slash == std::string::npos) ? std::string() : dst.substr(0, slash + 1);
    std::string leaf = (slash == std::string::npos) ? dst : dst.substr(slash + 1);
    std::string stem = leaf, ext;
    size_t dot = leaf.find_last_of('.');
    if (dot != std::string::npos && dot != 0) { stem = leaf.substr(0, dot); ext = leaf.substr(dot); }
    for (int n = 2; n < 1000; n++) {
        std::string cand = stem + " (" + std::to_string(n) + ")" + ext;
        if (taken) { if (!taken->count(cand)) return dir + cand; }
        else if (!fs_exists((dir + cand).c_str())) return dir + cand;
    }
    return dst;   // nothing free in a thousand tries: overwriting is what is left
}

// Run one copy or move over the plan's names. The names, not the pane's marks,
// decide what gets touched: the dialog showed a list, and a click while the
// overwrite confirm is up must not change what the operation acts on.
//
// The same confirm also decided what to do with the names that collided, and this
// is where that decision is honoured: skip leaves the target alone and counts
// itself, rename writes beside it. A skipped name is not a failure and is not a
// success either, so it is counted separately.
static int run_names_prog(AppPanes* app, const OpPlan& plan, fs_progress_fn cb, void* user,
                          int* skipped) {
    int ok = 0;
    if (skipped) *skipped = 0;
    for (size_t i = 0; i < plan.names.size(); i++) {
        std::string src = plan.src_dir + "/" + plan.names[i];
        std::string dst = plan_target(plan, i);
        if (fs_exists(dst.c_str())) {
            if (plan.overwrite == OpPlan::OW_SKIP) {
                if (skipped) (*skipped)++;
                continue;
            }
            if (plan.overwrite == OpPlan::OW_RENAME) dst = rename_beside(dst, nullptr);
        }
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

// The callback for the parts of a transfer that move no bytes: the walk that
// plans it and the delete that finishes a move. It reports nothing - the counters
// belong to the files, which the queue's threads move - and its false answer is
// what stops either of them after an Abort. Quitting asks every transfer to stop
// (app_panes_free), so the request arrives through the same flag.
static bool bg_abort_cb(long long, long long, int, int, void* user) {
    return !((BgTransfer*)user)->abort.load();
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
    // how long that took. How long that took is the transfer's own span, frozen
    // when the worker stopped: the row is still drawn minutes later, and a
    // duration measured against the clock counts up under a bar that says "done".
    if (t->state.load(std::memory_order_acquire) != BgTransfer::BG_RUNNING) {
        long long end_ms = t->t_end_ms.load(std::memory_order_acquire);
        double span = end_ms > 0 ? (double)end_ms / 1000.0 - t->t0 : 0.0;
        if (span < 0.0) span = 0.0;
        snprintf(out, n, "%s in %.0fs", done_s, span);
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
static void bg_run_remote(BgTransfer* t) {
    // The login and the plan travel inside the record now, because a stopped row
    // has to be able to say what it was doing. Both are written before the thread
    // starts and never touched again, so the worker only ever reads them.
    const NetSite& site = t->site;
    const OpPlan& plan = t->plan;
    // Pure string work, no globals, so it is safe to do here rather than hand the
    // result over with the plan.
    std::string droot, drpath;
    net_split_virtual(plan.dst_dir.c_str(), &droot, &drpath);

    // First the plan: every name becomes the files it holds, with their sizes, so
    // the row knows its whole total before a byte moves, and the queue's threads
    // can share the files out. Each name is a group, which is how the end of the
    // run knows which names arrived whole.
    const int n = (int)plan.names.size();
    t->begin(n);
    std::string err;
    int skipped = 0;
    std::set<std::string> remote_names;   // the upload's destination, as the server lists it
    bool have_listing = false;
    std::vector<char> planned((size_t)n, 0);
    std::vector<std::string> sources((size_t)n);
    std::vector<XferItem> items;
    for (int i = 0; i < n && !t->abort.load(); i++) {
        std::string base = plan.name_override.empty() ? plan.names[i] : plan.name_override;
        std::string perr;
        bool r;
        if (t->download) {
            std::string src = net_join_remote(plan.remote_prefix, plan.names[i]);
            std::string local = plan.dst_dir + "/" + base;
            // The destination of a download is a directory on this disk, so the
            // collision is asked of this disk.
            if (fs_exists(local.c_str())) {
                if (plan.overwrite == OpPlan::OW_SKIP) { skipped++; continue; }
                if (plan.overwrite == OpPlan::OW_RENAME)
                    local = rename_beside(local, nullptr);
            }
            sources[i] = src;
            // A resume is per file: the one with a `.part` continues from it and the
            // ones without are fetched whole. A restart clears the sidecars.
            r = xfer_plan_download(site, src, local, t, i, t->resume, &items, &perr);
        } else {
            std::string local = plan.src_dir + "/" + plan.names[i];
            std::string dst = drpath.empty() ? base : drpath + "/" + base;
            // The destination of an upload is on the server, and the server is the
            // only thing that knows what is already there. One listing covers every
            // name in the plan, which is what a per-name lookup cannot do.
            if (plan.overwrite != OpPlan::OW_OVERWRITE && !have_listing) {
                have_listing = true;
                std::vector<FileEntry> rows;
                std::string lerr;
                if (net_list(site, drpath, &rows, &lerr))
                    for (const FileEntry& fe : rows) remote_names.insert(fe.name);
            }
            if (have_listing && remote_names.count(base)) {
                if (plan.overwrite == OpPlan::OW_SKIP) { skipped++; continue; }
                if (plan.overwrite == OpPlan::OW_RENAME) {
                    std::string beside = rename_beside(dst, &remote_names);
                    remote_names.insert(beside.substr(beside.find_last_of('/') + 1));
                    dst = beside;
                }
            }
            sources[i] = local;
            // Here the offset is the size the server already holds, which is what a
            // stopped upload left behind on the far side.
            r = xfer_plan_upload(site, local, dst, t, i, t->resume, &items, &perr);
        }
        if (r) planned[i] = 1;
        else if (err.empty()) err = perr;
    }

    // The connection the plan was made over is not needed while the queue runs -
    // each of the queue's threads has its own - and a server that counts
    // connections per user should see only those.
    net_forget(&site);
    if (site.rar_order) xfer_order_rar(&items);
    if (!t->abort.load()) xfer_run(t, site, site.threads, std::move(items));

    // A name counts once every file it held has arrived, and only then is the
    // source of a move removed: the same order as before, now per name.
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (!planned[i] || !t->group_ok(i)) continue;
        ok++;
        if (plan.is_move && !t->abort.load()) {
            if (t->download) net_remove_tree(site, sources[i], bg_abort_cb, t, &err);
            else fs_delete_entry_progress(sources[i].c_str(), bg_abort_cb, t);
        }
    }
    if (err.empty()) {
        std::lock_guard<std::mutex> lk(t->mu);
        err = t->first_err;
    }
    t->ok_names = ok;
    t->skipped_names = skipped;
    if (!ok && !skipped)
        t->error = t->abort.load() ? "stopped before anything transferred"
                                   : (err.empty() ? std::string("transfer failed") : err);
    // The clock stops here. The row outlives the thread and stays on screen, so a
    // duration measured against the clock would keep counting up under a bar that
    // says "done". Written before the store below, which is what publishes it.
    t->t_end_ms = (long long)SDL_GetTicks();
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
        if (old == app->choice_transfer) continue;   // a question about it is open
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
    // The record keeps its own plan and its own login: a row that stopped is only
    // worth a Resume button if it still knows what it was doing and where.
    t->plan     = plan;
    t->site     = site;
    t->resumable = true;
    t->resume   = false;
    // Everything above is fixed before the thread starts, so the worker never
    // sees a half-built record.
    t->worker = std::thread(bg_run_remote, t);
    app->transfers.push_back(std::move(up));
    return t;
}

// Start a stopped row again, which is the whole reason the record keeps its plan.
// `restart` is the difference between continuing and starting over: a resume hands
// the transport the offsets it can pick up - the size of each `.part` for a
// download, the size the server reports for an upload - and a restart clears them
// so the transfer runs from nothing.
//
// The worker being replaced has run to completion: a state that is not BG_RUNNING
// is what its last store publishes, and the rule the struct carries is that only a
// finished worker is ever joined.
static void bg_resume_remote(AppPanes* app, BgTransfer* t, bool restart) {
    if (!t) return;
    if (t->state.load(std::memory_order_acquire) == BgTransfer::BG_RUNNING) return;
    if (t->worker.joinable()) t->worker.join();

    t->done = 0;
    t->total = 0;
    t->files_done = 0;
    t->files_total = 0;
    t->error.clear();
    t->ok_names = 0;
    t->t_end_ms = 0;
    t->settled = false;              // the finish has to be reported again
    t->dismiss = false;
    t->abort = false;
    t->resume = !restart;
    t->t0 = SDL_GetTicks() / 1000.0;
    t->state.store(BgTransfer::BG_RUNNING, std::memory_order_release);
    t->worker = std::thread(bg_run_remote, t);
    snprintf(app->status_msg, sizeof(app->status_msg), "%s the transfer to %s",
             restart ? "Restarting" : "Resuming", t->dst.c_str());
}

// Between frames, before anything is drawn. A transfer that has finished gets its
// destination refreshed and its result in the status bar exactly once; a row the
// user dismissed is joined and removed. The join happens here rather than in the
// widget so that no button press can wait on a thread inside an ImGui frame - the
// thread is already finished by then, but the ordering is the part that matters.
// ---------------------------------------------------------------------------
// .sfv verification: the worker, its row in the transfer strip, and its report.
// ---------------------------------------------------------------------------

enum { SFV_MODE_ASK = 0, SFV_MODE_ALWAYS, SFV_MODE_NEVER };

// What should happen when a download lands in a folder that holds a .sfv. The
// config's word, with the session's "Always" on top of it: a user who clicked
// Always once in a dialog is not stuck with it the next time the app starts.
static int sfv_mode(AppPanes* app) {
    if (app->sfv_always_session) return SFV_MODE_ALWAYS;
    const char* m = app->sfv_verify[0] ? app->sfv_verify : "ask";
    if (!strcmp(m, "always")) return SFV_MODE_ALWAYS;
    if (!strcmp(m, "never")) return SFV_MODE_NEVER;
    return SFV_MODE_ASK;
}

static std::string sfv_base(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// Which .sfv a folder means when it holds more than one. A set is usually named
// after itself - `pack.part1.rar` beside `pack.sfv` - so the one whose name shares
// the longest prefix with what was transferred wins; with no match, a single file is
// that file, and several are the shortest name, which is what a set ships.
static bool sfv_pick(const std::string& dir, const std::vector<std::string>& names,
                     std::string* out) {
    std::vector<std::string> list;
    if (!sfv_list(dir.c_str(), &list)) return false;

    std::string best = list[0];
    size_t best_score = 0;
    for (const std::string& n : names) {
        std::string stem = sfv_base(n);
        size_t dot = stem.find('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        if (stem.empty()) continue;
        for (const std::string& s : list) {
            std::string sbase = sfv_base(s);
            size_t sdot = sbase.find('.');
            if (sdot != std::string::npos) sbase = sbase.substr(0, sdot);
            if (sbase.empty()) continue;
            size_t score = 0;
            while (score < stem.size() && score < sbase.size() &&
                   tolower((unsigned char)stem[score]) == tolower((unsigned char)sbase[score]))
                score++;
            if (score > best_score) { best_score = score; best = s; }
        }
    }
    if (!best_score && list.size() > 1) {
        best = list[0];
        for (const std::string& s : list)
            if (s.size() < best.size()) best = s;
    }
    if (out) *out = dir + "/" + best;
    return true;
}

// The hashing runs here, one file at a time, with the row published as it goes so
// the strip can count up. A file the .sfv lists and the folder does not have is
// MISSING, which is a different answer from a checksum that does not match, and a
// file the folder has and the .sfv does not list is not reported at all: the .sfv
// says what it expects, and a folder with extra files is not its problem.
static void sfv_worker(AppPanes* app, SfvJob* job) {
    (void)app;
    SfvParse p;
    std::string err;
    if (!sfv_parse(job->sfv_path.c_str(), job->sfv_path.c_str(), &p, &err)) {
        std::lock_guard<std::mutex> lk(job->mu);
        job->err = err;
        job->state.store(2, std::memory_order_release);
        return;
    }
    job->total.store((long long)p.entries.size(), std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(job->mu);
        job->malformed = p.malformed;
    }
    for (size_t i = 0; i < p.entries.size() && !job->cancel.load(); i++) {
        const SfvEntry& e = p.entries[i];
        SfvRow row;
        row.name = e.name;
        row.want = e.crc;
        unsigned long got = 0;
        std::string ferr;
        std::string path = job->dir + "/" + e.name;
        if (!sfv_crc32_file(path.c_str(), &got, &ferr)) {
            row.status = SFV_MISSING;
        } else {
            row.got = got;
            row.status = (got == e.crc) ? SFV_OK : SFV_MISMATCH;
        }
        {
            std::lock_guard<std::mutex> lk(job->mu);
            job->rows.push_back(row);
        }
        job->done.fetch_add(1);
    }
    job->state.store(job->cancel.load() ? 2 : 1, std::memory_order_release);
}

static void sfv_job_start(AppPanes* app, const std::string& dir, const std::string& sfv_path,
                          const OpPlan* plan, const NetSite* site) {
    auto held = std::make_unique<SfvJob>();
    SfvJob* job = held.get();
    job->dir = dir;
    job->sfv_path = sfv_path;
    if (plan) { job->plan = *plan; job->has_plan = true; }
    if (site) job->site = *site;
    app->sfv_jobs.push_back(std::move(held));
    job->worker = std::thread(sfv_worker, app, job);
    snprintf(app->status_msg, sizeof(app->status_msg), "Verifying against %s",
             sfv_base(sfv_path).c_str());
}

// The summary of a finished run, once. A run with nothing wrong says so in the
// status line and opens no window: a report that is always correct is a report
// people learn to click past.
static void sfv_reap(AppPanes* app) {
    for (size_t i = 0; i < app->sfv_jobs.size();) {
        SfvJob* job = app->sfv_jobs[i].get();
        bool running = job->state.load(std::memory_order_acquire) == 0;
        if (running) { i++; continue; }
        if (job->worker.joinable()) job->worker.join();

        if (!job->settled) {
            job->settled = true;
            int ok = 0, bad = 0, missing = 0;
            {
                std::lock_guard<std::mutex> lk(job->mu);
                for (const SfvRow& r : job->rows) {
                    if (r.status == SFV_OK) ok++;
                    else if (r.status == SFV_MISSING) missing++;
                    else bad++;
                }
            }
            if (job->state.load() == 2 && !job->err.empty()) {
                snprintf(app->status_msg, sizeof(app->status_msg), "SFV: %s", job->err.c_str());
            } else if (job->cancel.load()) {
                snprintf(app->status_msg, sizeof(app->status_msg),
                         "SFV stopped after %lld of %lld files",
                         job->done.load(), job->total.load());
            } else {
                snprintf(app->status_msg, sizeof(app->status_msg), "%s  %d listed  %d ok  %d bad",
                         sfv_base(job->sfv_path).c_str(), (int)job->rows.size(), ok, bad + missing);
            }
            job->report_open = (bad + missing > 0) || job->malformed > 0 ||
                               job->state.load() == 2;
        }

        if (!job->report_open || job->dismiss.load()) {
            app->sfv_jobs.erase(app->sfv_jobs.begin() + (long)i);
            continue;
        }
        i++;
    }
}

// The row in the strip while the hashing runs. It is a row rather than a window
// because the strip is where a user already looks for work the app is doing, and a
// 4 GB file takes long enough to be worth watching.
static void render_sfv_jobs(AppPanes* app) {
    for (auto& held : app->sfv_jobs) {
        SfvJob* job = held.get();
        if (job->state.load(std::memory_order_acquire) != 0) continue;
        long long total = job->total.load(), done = job->done.load();
        ImGui::PushID(job);
        ImGui::Text("verify %lld of %lld  %s", done, total, sfv_base(job->sfv_path).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop##sfv")) job->cancel.store(true);
        ImGui::PopID();
    }
}

// The plan "re-fetch the bad ones" runs: the names that failed, out of the transfer
// that fetched them, overwriting the copies that failed. Split out of the report
// window so the rule is testable without a display, and so the button cannot invent a
// fetch from a verification that had no source to fetch from.
static bool sfv_retry_plan(const SfvJob* job, const std::vector<SfvRow>& rows, OpPlan* out) {
    if (!job->has_plan || job->plan.remote.empty()) return false;
    out->remote = job->plan.remote;
    out->dst_dir = job->plan.dst_dir;
    // The bad copy is the thing being replaced, and asking again would ask about
    // files the user has already been shown failing.
    out->overwrite = OpPlan::OW_OVERWRITE;
    out->names.clear();
    for (const SfvRow& r : rows) {
        if (r.status == SFV_OK) continue;
        for (const std::string& n : job->plan.names)
            if (n == r.name || sfv_base(n) == sfv_base(r.name)) out->names.push_back(n);
    }
    return !out->names.empty();
}

// The report. Names, what the .sfv expected, what the file has, and the one button
// that can do something about it when the verification came out of a transfer that
// is still in this session.
static void render_sfv_reports(AppPanes* app) {
    static const int SFV_MAX_ROWS = 100;
    for (auto& held : app->sfv_jobs) {
        SfvJob* job = held.get();
        if (!job->report_open) continue;
        int ok = 0, bad = 0, missing = 0;
        std::vector<SfvRow> rows;
        int malformed = 0;
        {
            std::lock_guard<std::mutex> lk(job->mu);
            rows = job->rows;
            malformed = job->malformed;
        }
        for (const SfvRow& r : rows) {
            if (r.status == SFV_OK) ok++;
            else if (r.status == SFV_MISSING) missing++;
            else bad++;
        }

        char title[128];
        snprintf(title, sizeof(title), "SFV report - %s", sfv_base(job->sfv_path).c_str());
        ImGui::Begin(title);
        ImGui::Text("%s   %d listed   %d ok   %d mismatch   %d missing",
                    sfv_base(job->sfv_path).c_str(), (int)rows.size(), ok, bad, missing);
        if (malformed > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                               "%d line%s in the .sfv were not a name and a checksum",
                               malformed, malformed == 1 ? "" : "s");
        if (!job->err.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", job->err.c_str());
        ImGui::Separator();

        int shown = 0;
        for (const SfvRow& r : rows) {
            if (r.status == SFV_OK) continue;
            if (shown >= SFV_MAX_ROWS) break;
            char want[16], got[16];
            sfv_format_crc(r.want, want, sizeof(want));
            sfv_format_crc(r.got, got, sizeof(got));
            if (r.status == SFV_MISSING)
                ImGui::Text("%-40s  MISSING    want %s", r.name.c_str(), want);
            else
                ImGui::Text("%-40s  MISMATCH   want %s  got %s", r.name.c_str(), want, got);
            shown++;
        }
        if ((int)rows.size() > shown)
            ImGui::Text("(%d more row%s not shown)", (int)rows.size() - shown,
                        (rows.size() - (size_t)shown) == 1 ? "" : "s");
        if (rows.empty() && job->err.empty())
            ImGui::Text("The .sfv lists nothing.");

        ImGui::Separator();
        bool bad_files = (bad + missing) > 0;
        OpPlan retry;
        bool can_retry = bad_files && sfv_retry_plan(job, rows, &retry);
        if (can_retry) {
            ImGui::Text("Re-fetch pulls the %d file%s that failed from %s again.",
                        (int)retry.names.size(), retry.names.size() == 1 ? "" : "s",
                        job->plan.remote.c_str());
            if (ImGui::Button("Re-fetch the bad ones")) {
                bg_start_remote_plan(app, retry);
                job->dismiss.store(true);
            }
        } else if (bad_files) {
            ImGui::Text("No transfer in this session fetched these files, so there is "
                        "nothing here to re-fetch them from.");
        }
        if (ImGui::Button("Close")) job->dismiss.store(true);
        ImGui::End();
    }
}

// A download that landed something, was not aborted, and has a local destination is
// where the offer belongs: the files are already here and the .sfv arrived with
// them, which is the only moment the check is on the user's mind.
static void sfv_offer_after_download(AppPanes* app, BgTransfer* t) {
    if (!t->download || !t->ok_names || t->abort.load()) return;
    if (sfv_mode(app) == SFV_MODE_NEVER) return;
    std::string sfv;
    if (!sfv_pick(t->plan.dst_dir, t->plan.names, &sfv)) return;
    if (sfv_mode(app) == SFV_MODE_ALWAYS) {
        sfv_job_start(app, t->plan.dst_dir, sfv, &t->plan, nullptr);
        return;
    }
    app->sfv_pending_path = sfv;
    app->sfv_pending_dir = t->plan.dst_dir;
    app->sfv_pending_transfer = t;
    snprintf(app->confirm_text, sizeof(app->confirm_text),
             "%s is in %s.\nVerify the %d transferred file%s against it?\n\n"
             "[Verify]  [Not now]  [Always]      (press V / N / A)",
             sfv_base(sfv).c_str(), t->plan.dst_dir.c_str(), t->ok_names,
             t->ok_names == 1 ? "" : "s");
    app->confirm = CONF_SFV_VERIFY;
    app->pending_confirm_answer = CONF_ANS_NONE;
}

// Commands → Verify SFV: the same job on the focused pane's directory, with no
// fetching involved. A pane inside an archive or on a site is refused with a reason,
// because "the directory the pane shows" is not a directory there, and a check that
// quietly hashed nothing would be worse than one that says it did not run.
static void sfv_run_for_pane(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (active.list_mode == LIST_ARCHIVE) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "This pane is inside an archive: verify a .sfv from a real directory");
        return;
    }
    if (in_remote(active)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "This pane is on a site: Ctrl+D leaves it, then a .sfv can be verified");
        return;
    }
    std::string sfv;
    std::vector<std::string> none;
    if (!sfv_pick(active.current_dir, none, &sfv)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "No .sfv in %s", active.current_dir);
        return;
    }
    sfv_job_start(app, active.current_dir, sfv, nullptr, nullptr);
}

static void bg_reap(AppPanes* app) {
    for (size_t i = 0; i < app->transfers.size();) {
        BgTransfer* t = app->transfers[i].get();
        bool running = t->state.load(std::memory_order_acquire) == BgTransfer::BG_RUNNING;
        if (!running && !t->settled.exchange(true)) {
            char skip_note[32] = "";
            if (t->skipped_names)
                snprintf(skip_note, sizeof(skip_note), "  %d left alone", t->skipped_names);
            if (t->ok_names)
                snprintf(app->status_msg, sizeof(app->status_msg),
                         "%s %d item%s to %s%s%s", t->is_move ? "Moved" : "Copied", t->ok_names,
                         t->ok_names == 1 ? "" : "s", t->dst.c_str(),
                         t->abort.load() ? " (aborted)" : "", skip_note);
            else if (t->skipped_names)
                snprintf(app->status_msg, sizeof(app->status_msg),
                         "Nothing transferred to %s  %d target%s left alone",
                         t->dst.c_str(), t->skipped_names,
                         t->skipped_names == 1 ? "" : "s");
            else
                snprintf(app->status_msg, sizeof(app->status_msg), "%s failed: %s",
                         t->is_move ? "Move" : "Copy", t->error.c_str());
            refresh_for_destination(app, t->dst);
            // The files are here and the .sfv arrived with them: this is the only
            // moment the check is on the user's mind. A transfer that was aborted
            // deliberately does not get asked, because the answer would be about a
            // half-finished folder.
            sfv_offer_after_download(app, t);
        }
        if (!running && t->dismiss.load()) {
            // A row the choice dialog is asking about stays put: the answer arrives
            // in a later frame, and a record that has been erased is not something
            // that answer can be applied to.
            if (t == app->choice_transfer) { i++; continue; }
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
    // The rows are about to go away, and a question about one of them goes with
    // them rather than pointing at memory that has been freed.
    app->choice = CHOICE_NONE;
    app->choice_transfer = nullptr;
    app->choice_pick = -1;
    app->choice_text[0] = '\0';
    for (std::unique_ptr<BgTransfer>& t : app->transfers) t->abort = true;
    for (std::unique_ptr<BgTransfer>& t : app->transfers) {
        if (t->worker.joinable()) t->worker.join();
    }
    app->transfers.clear();
    // Every row has returned from the queue, so what is left there are threads
    // lingering for more work. They hold connections, and the library they belong
    // to is about to be released.
    xfer_shutdown();
}

// The queue as a whole, for the line over the rows: the bar, what is left, and
// with the threads' speed, when it will be done.
struct QueueSummary {
    long long done = 0;    // bytes the batch's rows have
    long long total = 0;   // bytes the batch adds up to
    long long left = 0;    // bytes the running rows still owe
    int running = 0;       // rows still running
};

// Which rows are in the batch is decided here, and so is when a batch starts and
// ends: it starts with the first row running after an idle queue and ends when no
// row runs. A finished row counts what it moved and nothing more, so a row that
// failed halfway does not hold the bar short of the end forever.
static QueueSummary bg_queue_summary(AppPanes* app) {
    QueueSummary q;
    double first = 0.0;
    for (const std::unique_ptr<BgTransfer>& t : app->transfers) {
        if (t->state.load(std::memory_order_acquire) != BgTransfer::BG_RUNNING) continue;
        q.running++;
        if (first == 0.0 || t->t0 < first) first = t->t0;
    }
    if (!q.running) { app->queue_t0 = 0.0; return q; }
    if (app->queue_t0 == 0.0 || first < app->queue_t0) app->queue_t0 = first;
    for (const std::unique_ptr<BgTransfer>& t : app->transfers) {
        bool running = t->state.load(std::memory_order_acquire) == BgTransfer::BG_RUNNING;
        if (!running && t->t0 < app->queue_t0) continue;   // from an earlier batch
        long long done = t->done.load(), total = t->total.load();
        q.done += done;
        if (running) {
            q.total += total > done ? total : done;
            q.left += total > done ? total - done : 0;
        } else {
            q.total += done;
        }
    }
    return q;
}

// Bytes per second in the unit transfer windows use, and in the same decimal
// megabytes human_size() writes sizes in, so "3.0M of 9.0M at 2.0 MB/s" adds up.
static void fmt_rate(double bps, char* out, size_t n) {
    if (!(bps > 0.0)) bps = 0.0;
    snprintf(out, n, "%.1f MB/s", bps / 1e6);
}

// The line beside the queue's bar: how much of the batch has moved, when the rest
// will have, and the speed of the threads moving it - the sum of what each one is
// doing now, which is the number a stall shows up in at once.
static void bg_queue_line(const QueueSummary& q, double rate, int busy, char* out, size_t n) {
    char done_s[32], total_s[32], rate_s[32], eta[32] = "";
    human_size_trim((off_t)q.done, done_s, sizeof(done_s));
    human_size_trim((off_t)q.total, total_s, sizeof(total_s));
    fmt_rate(rate, rate_s, sizeof(rate_s));
    if (rate > 1.0 && q.left > 0) fmt_secs((double)q.left / rate, eta, sizeof(eta));
    snprintf(out, n, "%s of %s  eta %s  %s  %d thread%s", done_s, total_s,
             eta[0] ? eta : "-", rate_s, busy, busy == 1 ? "" : "s");
}

// How much room the transfer strip needs, so the panes can be given what is left
// of the window before the strip is drawn. One row per transfer plus a header, at
// most seven rows before the list scrolls.
static float bg_strip_height(const AppPanes* app, float frame) {
    if (app->transfers.empty()) return 0.0f;
    int rows = 1 + (int)app->transfers.size();
    if (rows > 7) rows = 7;
    // The queue's own line over the table, while anything is running.
    for (const std::unique_ptr<BgTransfer>& t : app->transfers)
        if (t->state.load(std::memory_order_acquire) == BgTransfer::BG_RUNNING) { rows++; break; }
    return 6.0f + (float)rows * frame;
}

// The transfer list, between the panes and the command line. It claims no space
// while it is empty, so a session that never touches the network never sees it.
static void render_transfers(AppPanes* app, float h) {
    if (app->transfers.empty() && app->sfv_jobs.empty()) return;
    ImGui::Spacing();
    // The queue: one bar for everything this burst of work is moving, and the
    // speed of the threads moving it. It is a line of its own over the table, as
    // wide as the strip: in a table column it was cut off at the buttons. Sampled
    // every frame so the meter has the history a "current" speed is measured over.
    std::vector<XferThreadStat> threads;
    int queued = 0;
    xfer_snapshot(&threads, &queued);
    double rate = app->queue_meter.update(SDL_GetTicks() / 1000.0, threads);
    QueueSummary q = bg_queue_summary(app);
    if (q.running) {
        float frac = q.total > 0 ? (float)((double)q.done / (double)q.total) : 0.0f;
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "queue %d%%", (int)(frac * 100.0f + 0.5f));
        ImGui::ProgressBar(frac, ImVec2(150.0f, 0.0f), overlay);
        ImGui::SameLine();
        int busy = 0;
        for (const XferThreadStat& s : threads) busy += s.file.empty() ? 0 : 1;
        char line[256];
        bg_queue_line(q, rate, busy, line, sizeof(line));
        ImGui::TextUnformatted(elide_middle(line, ImGui::GetContentRegionAvail().x).c_str());
        // Each thread's own share of the sum, and what it is moving.
        if (ImGui::IsItemHovered()) {
            char head[64];
            snprintf(head, sizeof(head), "%d file%s waiting in the queue", queued,
                     queued == 1 ? "" : "s");
            std::string tip = head;
            for (size_t i = 0; i < threads.size(); i++) {
                const XferThreadStat& s = threads[i];
                char r[32];
                fmt_rate(app->queue_meter.rate[s.id], r, sizeof(r));
                char row[512];
                snprintf(row, sizeof(row), "\nthread %d  %-12s %s", (int)i + 1, r,
                         s.file.empty() ? "(waiting for work)" : s.file.c_str());
                tip += row;
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
    if (app->transfers.size() > 6) flags |= ImGuiTableFlags_ScrollY;
    float table_h = h - 6.0f - (q.running ? ImGui::GetFrameHeightWithSpacing() : 0.0f);
    if (!ImGui::BeginTable("bg_transfers", 5, flags, ImVec2(0, table_h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 40.0f);   // get / put
    ImGui::TableSetupColumn("Transfer", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 150.0f);  // the bar
    // The numbers column is as wide as the longest line bg_line() writes in the
    // font in use, so a larger font widens it instead of cutting it off.
    float numbers_w = ImGui::CalcTextSize("999.9M of 999.9M   999.9M left   999.9M/s   eta 59m59s").x +
                      ImGui::GetStyle().CellPadding.x * 2.0f;
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, numbers_w);  // the numbers
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 140.0f);  // the buttons

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
            // A row that stopped with work left is the one worth starting again,
            // and only a row that still holds its plan and its login can be. A
            // finished one has nothing left to do.
            bool stopped = st == BgTransfer::BG_ABORTED || st == BgTransfer::BG_FAILED;
            if (stopped && t->resumable && ImGui::SmallButton("Resume")) {
                char got[32], owed[32];
                human_size((off_t)t->done.load(), got, sizeof(got));
                human_size((off_t)t->total.load(), owed, sizeof(owed));
                app->choice = CHOICE_RESUME;
                app->choice_transfer = t;
                snprintf(app->choice_text, sizeof(app->choice_text),
                         "%s to %s stopped with %s of %s done. Continue from what is already "
                         "there, throw it away and run again, or leave it?",
                         t->download ? "Download" : "Upload", t->dst.c_str(), got, owed);
                app->choice_pick = -1;
            }
            if (ImGui::SmallButton("clear")) t->dismiss = true;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    // A .sfv that is still hashing gets a line under the transfer rows rather than a
    // row inside the table: it has no bytes, no bar and no source, and inventing
    // columns for it would make the table describe two different kinds of work.
    render_sfv_jobs(app);
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
        // A skip is decided here rather than inside the extractor: the tool is told
        // which members to write, and not asking for the one whose target is already
        // there is the same answer without needing a "do not overwrite" flag per
        // tool - which zip, tar and 7z each spell differently. Rename is not offered
        // for an archive at all: the names inside it are the extractor's to write.
        std::vector<std::string> raws = plan.raw_names;
        int skipped = 0;
        if (plan.overwrite == OpPlan::OW_SKIP) {
            raws.clear();
            for (size_t i = 0; i < plan.raw_names.size(); i++) {
                if (fs_exists(plan_target(plan, i).c_str())) { skipped++; continue; }
                raws.push_back(plan.raw_names[i]);
            }
        }
        if (raws.empty()) {
            snprintf(app->status_msg, sizeof(app->status_msg),
                     "Nothing extracted from %s  %d target%s left alone", plan.archive.c_str(),
                     skipped, skipped == 1 ? "" : "s");
            refresh_for_destination(app, plan.dst_dir);
            return;
        }
        arch_prog_begin(app, plan.archive.c_str(), plan.dst_dir.c_str());
        std::string err;
        bool ok = arch_extract_members(plan.archive.c_str(), plan.dst_dir.c_str(), raws,
                                       copy_progress_cb, app, &err);
        app->prog_active = false;
        if (ok)
            snprintf(app->status_msg, sizeof(app->status_msg), "Copied %d member%s into %s%s%s%s",
                     (int)raws.size(), raws.size() == 1 ? "" : "s", plan.dst_dir.c_str(),
                     plan.archive_prefix.empty() ? "" : " (archive paths preserved)",
                     app->prog_abort ? " (aborted)" : "",
                     skipped ? "  (left alone)" : "");
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
    int skipped = 0;
    int ok = run_names_prog(app, plan, copy_progress_cb, app, &skipped);
    app->prog_active = false;
    // A skip is an answer, not a failure: the target the user chose to leave alone
    // is still there, and what ran is what they asked for. A plan where every name
    // was skipped says so rather than saying "failed".
    char skip_note[32] = "";
    if (skipped) snprintf(skip_note, sizeof(skip_note), "  %d skipped", skipped);
    if (ok)
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "%s %d item%s to %s%s%s", plan.is_move ? "Moved" : "Copied",
                 ok, ok == 1 ? "" : "s", plan.dst_dir.c_str(),
                 app->prog_abort ? " (aborted)" : "", skip_note);
    else if (skipped)
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Nothing copied to %s  %d target%s left alone",
                 plan.dst_dir.c_str(), skipped, skipped == 1 ? "" : "s");
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
    app->op_focus = OF_FIELD;
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
    app->op_focus = OF_FIELD;
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

// TC Ctrl+P: put the pane's path in the console. Text that is already there is
// kept and the path appended, so "rm " + path works; the field is shorter than a
// path can be, so a deep directory truncates inside it instead of overflowing it.
static void console_put_path(AppPanes* app, Pane& pane) {
    console_show(app, true);
    char line[512];
    snprintf(line, sizeof(line), "%s", app->console_input);
    size_t used = strlen(line);
    if (used && line[used - 1] != ' ') {
        if (used + 1 < sizeof(line)) line[used++] = ' ';
    }
    line[used] = '\0';
    // Inside an archive the pane's path is virtual and no shell would accept it,
    // so the archive file itself is what is useful to put on the line.
    snprintf(line + used, sizeof(line) - used, "%s",
             in_archive(pane) ? pane.archive_path : pane.current_dir);
    console_set_input(app, line);
    snprintf(app->status_msg, sizeof(app->status_msg), "Path into the console");
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

static void do_paste_archive_now(AppPanes* app, OpPlan::Overwrite ow) {
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;
    const char* arch = app->clipboard.archive;
    std::vector<ArchEntry> members;
    std::string lerr;
    arch_list(arch, &members, &lerr);   // best effort: fall back to the display path
    std::vector<std::string> raws;
    int skipped = 0;
    for (const std::string& m : app->clipboard.paths) {
        const ArchEntry* e = arch_find(members, m.c_str());
        const std::string& raw = e ? e->raw : m;
        // A skip is decided here rather than inside the extractor: the top-level
        // name the pane showed is the thing that collides, and not asking the tool
        // for that subtree is the same answer without needing a flag per tool.
        if (ow == OpPlan::OW_SKIP) {
            // The same rule the plan-based extract uses: the member's own path under
            // the destination is the thing that collides.
            std::string there = std::string(active.current_dir) + "/" + m;
            if (fs_exists(there.c_str())) { skipped++; continue; }
        }
        raws.push_back(raw);
    }
    if (raws.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Nothing extracted from %s  %d target%s left alone", arch, skipped,
                 skipped == 1 ? "" : "s");
        refresh_pane(&active, active.current_dir);
        refresh_pane(&other, other.current_dir);
        return;
    }
    arch_prog_begin(app, arch, active.current_dir);
    std::string err;
    bool ok = arch_extract_members(arch, active.current_dir, raws, copy_progress_cb, app, &err);
    app->prog_active = false;
    if (ok) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "Extracted %d member%s into %s%s", (int)raws.size(), raws.size() == 1 ? "" : "s",
                 active.current_dir, skipped ? "  (extracted over what was there)" : "");
        app->clipboard.paths.clear();
        app->clipboard.archive[0] = '\0';
    } else {
        snprintf(app->status_msg, sizeof(app->status_msg), "Extract failed: %s", err.c_str());
    }
    refresh_pane(&active, active.current_dir);
    refresh_pane(&other, other.current_dir);
}

static void do_paste_now(AppPanes* app, OpPlan::Overwrite ow) {
    if (app->clipboard.archive[0]) { do_paste_archive_now(app, ow); return; }
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other  = app->left_focus ? app->right : app->left;
    if (app->clipboard.paths.empty()) return;

    prog_begin(app, active.current_dir, active.current_dir, app->clipboard.cut);
    path_parent_dir(app->clipboard.paths[0], app->prog_src, sizeof(app->prog_src));
    snprintf(app->prog_dst, sizeof(app->prog_dst), "%s", active.current_dir);

    int ok = 0;
    int skipped = 0;
    StringList failed;   // a cut keeps whatever did not move so it can be retried
    for (size_t i = 0; i < app->clipboard.paths.size(); i++) {
        const std::string& src = app->clipboard.paths[i];
        char dst[4100];
        snprintf(dst, sizeof(dst), "%s/%s", active.current_dir, path_basename(src));
        if (fs_exists(dst)) {
            if (ow == OpPlan::OW_SKIP) { skipped++; continue; }
            if (ow == OpPlan::OW_RENAME) {
                std::string beside = rename_beside(dst, nullptr);
                snprintf(dst, sizeof(dst), "%s", beside.c_str());
            }
        }
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
             "%s %d entr%s into %s%s", app->clipboard.cut ? "Moved" : "Copied",
             ok, ok == 1 ? "y" : "ies", active.current_dir,
             skipped ? "  (target left alone)" : "");
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
                 "%d target%s already exist%s in %s. Overwrite them, leave them alone, "
                 "or paste beside them?", existing, existing == 1 ? "" : "s",
                 existing == 1 ? "s" : "", active.current_dir);
        return;
    }
    do_paste_now(app, OpPlan::OW_OVERWRITE);
}

// F4 launches a terminal emulator to host the editor; answering a confirm is where a
// deferred launch runs, so the pair is declared together.
static void spawn_terminal_editor(AppPanes* app, const char* fullpath);

// Which confirms are three-answer questions and which are plain yes/no. The overwrite
// questions take Overwrite / Skip / Rename and treat Esc as "cancel the whole
// operation"; the yes/no ones take Y / N and treat Esc as No. Asked as a question about
// the kind rather than written as `confirm != CONF_DELETE`, because a yes/no confirm
// added later would otherwise inherit the overwrite keys without anyone noticing.
static bool confirm_is_overwrite(int confirm) {
    return confirm == CONF_OVERWRITE || confirm == CONF_OVERWRITE_MOVE ||
           confirm == CONF_OVERWRITE_PASTE || confirm == CONF_OVERWRITE_EXTRACT;
}

static void confirm_answer(AppPanes* app, int answer) {
    int confirm = app->confirm;
    app->confirm = CONF_NONE;
    app->confirm_text[0] = '\0';

    switch (confirm) {
    case CONF_DELETE:
        if (answer == CONF_ANS_YES) do_delete(app);
        break;
    case CONF_OVERWRITE:
    case CONF_OVERWRITE_MOVE:
    case CONF_OVERWRITE_EXTRACT:
        // The choice goes into the plan, because the plan is what every runner
        // downstream already reads: the local copy, the extractor, and the network
        // worker all take the same record, and a choice only one of them honoured
        // would be a choice that changes shape on the way.
        if (answer == CONF_ANS_OVERWRITE) app->op_plan.overwrite = OpPlan::OW_OVERWRITE;
        else if (answer == CONF_ANS_SKIP) app->op_plan.overwrite = OpPlan::OW_SKIP;
        else if (answer == CONF_ANS_RENAME) app->op_plan.overwrite = OpPlan::OW_RENAME;
        else return;                       // Esc: the operation is cancelled outright
        run_op_plan(app);
        break;
    case CONF_OVERWRITE_PASTE:
        if (answer == CONF_ANS_OVERWRITE) do_paste_now(app, OpPlan::OW_OVERWRITE);
        else if (answer == CONF_ANS_SKIP) do_paste_now(app, OpPlan::OW_SKIP);
        else if (answer == CONF_ANS_RENAME) do_paste_now(app, OpPlan::OW_RENAME);
        break;
    case CONF_SFV_VERIFY:
        // "Always" is a session setting and not a config write: a user who clicked
        // it once in a dialog is not stuck with it the next time the app starts, and
        // the config line is the place for a decision that is meant to last.
        if (answer == CONF_ANS_ALWAYS) {
            app->sfv_always_session = true;
            snprintf(app->status_msg, sizeof(app->status_msg),
                     "Verifying after every download for this session "
                     "(sfv_verify in the config file makes it permanent)");
        }
        if ((answer == CONF_ANS_VERIFY || answer == CONF_ANS_ALWAYS) && !app->sfv_pending_path.empty()) {
            BgTransfer* src = app->sfv_pending_transfer;
            if (src) sfv_job_start(app, src->plan.dst_dir, app->sfv_pending_path, &src->plan, nullptr);
            else sfv_job_start(app, app->sfv_pending_dir, app->sfv_pending_path, nullptr, nullptr);
        }
        app->sfv_pending_path.clear();
        app->sfv_pending_dir.clear();
        app->sfv_pending_transfer = nullptr;
        break;
    case CONF_EDIT_BINARY:
        // The question exists because a text editor over a shared library produces
        // gibberish and a save can destroy the file. The path is the one the question
        // was asked about, not whatever the cursor moved to while it was open.
        if (answer == CONF_ANS_YES && app->pending_edit_path[0]) {
            spawn_terminal_editor(app, app->pending_edit_path);
        } else {
            snprintf(app->status_msg, sizeof(app->status_msg), "Not edited: %s",
                     app->pending_edit_path[0] ? app->pending_edit_path : "the file");
        }
        app->pending_edit_path[0] = '\0';
        break;
    default: break;
    }
}

// Deferred actions from menu/context-menu/dialog widgets: these fire inside
// the ImGui frame, but the operations below block and pump their own progress
// frames, so they must run between frames.
void app_panes_run_pending(AppPanes* app) {
    // The answer to a three-answer dialog. It is taken here rather than where it
    // was set because the answer starts a thread, and a thread is not started
    // inside a draw call - the same reason a confirm's Yes is consumed here.
    if (app->choice != CHOICE_NONE && app->choice_pick >= 0) {
        int pick = app->choice_pick;
        BgTransfer* t = app->choice_transfer;
        app->choice = CHOICE_NONE;
        app->choice_transfer = nullptr;
        app->choice_pick = -1;
        app->choice_text[0] = '\0';
        if (pick == 0) bg_resume_remote(app, t, false);
        else if (pick == 1) bg_resume_remote(app, t, true);
        // Skip leaves the row exactly as it is, which is an answer too.
    }
    if (app->pending_confirm_answer != CONF_ANS_NONE) {
        int answer = app->pending_confirm_answer;
        app->pending_confirm_answer = CONF_ANS_NONE;
        confirm_answer(app, answer);
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
        // Removed here rather than by `rm -rf` through a shell: there is no rm behind
        // Windows' system(), and no quoting to get wrong this way.
        fs_delete_entry(app->viewer_tmp_dir);
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
    } else {
        // The same alphabet question the quick view asks, because a file that is viewed
        // rather than previewed should not be the one place that still shows the garbage.
        std::string text(viewer_buf, (size_t)n);
        if (text_decide_codepage(text, text_codepage_from_name(app->preview_codepage)) ==
            TEXT_CODEPAGE_CP437) {
            std::string drawn = text_cp437_to_utf8(text);
            // UTF-8 is longer than the bytes it came from and this buffer is fixed, so
            // a transcode that would not fit is not attempted. What is on screen is one
            // alphabet or the other, never half of one.
            if (drawn.size() + 1 <= sizeof(viewer_buf))
                memcpy(viewer_buf, drawn.c_str(), drawn.size() + 1);
        }
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
    char tmpl[4096];
    snprintf(tmpl, sizeof(tmpl), "%s/mc-clone-ftp-XXXXXX", plat_temp_dir().c_str());
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

// Enter and Alt+F3: hand the entry to the desktop's default handler. Through a
// pointer so the tests can see what would have been opened without starting it.
static bool (*g_open_default)(const char*, std::string*) = plat_open_default;

static void open_with_default(AppPanes* app, const char* fullpath) {
    std::string err;
    if (!g_open_default(fullpath, &err)) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Cannot open %s: %s", fullpath, err.c_str());
        return;
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "Open with default: %s", fullpath);
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
        char parent[4096];
        pane_parent(pane.current_dir, parent, sizeof(parent));
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
        snprintf(full, sizeof(full), "\"%s/%s\"", pane.current_dir, rel);
        // Enter on an executable runs it in the pane's directory and the answer
        // goes to the console, which is where a command's output lives now. The
        // window opens so the output is visible, but the keyboard stays with the
        // pane: a program that takes a minute should not take the file manager
        // with it, and it no longer does - it runs on the console's worker.
        console_show(app, false);
        console_start(app, full);
    } else if (!in_remote(pane)) {
        // Any other file opens in the program the desktop has for it, as a double
        // click in Explorer or Total Commander does: a .png in the image viewer, a
        // .mkv in the video player. F3 and Shift+F3 are the built-in viewer.
        open_with_default(app, full);
    } else {
        view_entry(app, pane, idx);
    }
}

// F4: launch $EDITOR inside a terminal emulator so it gets a TTY.
//
// The command line is not the same for every terminal - see src/terminal.h - and the
// old version of this looked for five names, on a machine whose only terminal was
// alacritty, and then reported the failure by naming the editor: "No terminal emulator
// found for vim", which reads as a vim problem and is a terminal problem.
static void spawn_terminal_editor(AppPanes* app, const char* fullpath) {
    TermLaunch launch = term_find_launch(app->term_pref, fullpath);
    if (launch.argv.empty()) {
        snprintf(app->status_msg, sizeof(app->status_msg), "%s", launch.note.c_str());
        return;
    }
    std::string err;
    if (!plat_spawn_detached(launch.argv, &err)) {
        snprintf(app->status_msg, sizeof(app->status_msg), "Cannot run %s: %s",
                 launch.argv[0].c_str(), err.c_str());
        return;
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "Editing %s with %s in %s",
             fullpath, term_editor_name(), launch.name.c_str());
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

    // A `.so` is not text, and vim over it produces mojibake that a careless write turns
    // into a destroyed library. Say what the file is and ask, rather than opening it.
    // The decision is made from the bytes, so a `.txt` with ELF magic in it is asked
    // about too and a `.so` that is really a script is not.
    char why[160];
    if (fs_is_binary_file(full, why, sizeof why)) {
        app->confirm = CONF_EDIT_BINARY;
        snprintf(app->confirm_text, sizeof(app->confirm_text),
                 "%s is %s, not text. Open it in %s anyway?",
                 e->name, why, term_editor_name());
        snprintf(app->pending_edit_path, sizeof(app->pending_edit_path), "%s", full);
        return;
    }
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
    const std::string home = plat_home_dir();
    if (!home.empty() && strcmp(dir, home.c_str()) == 0) { snprintf(buf, n, "~"); return buf; }
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
    if (!in_remote(pane) && !in_archive(pane) && ImGui::MenuItem("Add to launch bar")) {
        char full[4300];
        snprintf(full, sizeof(full), "%s/%s", pane.current_dir, entry_rel(pane, pane.selected));
        launchbar_add(app, full);
        app->show_launchbar = true;
    }
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
        // Decided once, before the button. Clicking a tab changes which tab is
        // active, and a second `t == pane.tab_active` after the button would be true
        // for the tab that was just activated and false for the one that was styled -
        // a pop with nothing behind it, which ImGui answers by asserting.
        const bool is_active = (t == pane.tab_active);
        if (is_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, mc_theme_colors()->title_focused);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mc_theme_colors()->title_focused_hov);
        }
        if (ImGui::SmallButton(lbl)) tab_switch(&pane, t);
        if (is_active) ImGui::PopStyleColor(2);
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

// How much of a picture is read. A preview is a look, not an import: past this the
// file is refused rather than slurped, and the cap is what keeps a 200 MB scan from
// being decoded to answer one F3.
static const size_t k_qv_image_max_bytes = 32 * 1024 * 1024;
// Pictures kept, oldest first. Deliberately nothing like the text cache's 32: a
// decoded 4032x3024 photograph is 48 MB of pixels, so this is bounded by bytes as
// well as by count.
static const size_t k_qv_image_cache_max = 4;
static const size_t k_qv_image_cache_bytes = 96 * 1024 * 1024;
// The longest side a texture is made at. A 4032x3024 photo shown in a 700x525 window
// does not need 12 million texels, and shrinking once here costs one pass instead of
// a resample every frame - and the window is resized with the mouse.
static const int k_qv_tex_max_px = 2048;

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
    // Whether this row may turn out to be a picture, and may therefore be read
    // further than a screenful. Decided when the job is made, because it changes how
    // much is read, and the thread must not have to ask the app anything.
    bool want_image = false;
    // Which alphabet to read the bytes in. Same reason: decided here, on the UI
    // thread, from the config, so the worker never has to ask the app.
    TextCodepage codepage = TEXT_CODEPAGE_AUTO;
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
//
// Then the alphabet. ImGui reads text as UTF-8, so a UTF-8 file or a plain ASCII one
// is already right. A file from the DOS world - an .nfo, a .diz, ANSI art - is not:
// its box drawing is the single bytes 0xB0 to 0xDF, which are not valid UTF-8 and
// arrive as garbage, a border of ─ turning into two characters of nothing. AUTO
// consults the rule in text.h about the shape of the high bytes; utf8 and cp437 are
// the user overriding that call in either direction, which is what a file the rule
// misreads needs.
static bool qv_to_text(std::string* body, TextCodepage page = TEXT_CODEPAGE_AUTO) {
    if (body->find('\0') != std::string::npos) return false;
    for (size_t i = body->find('\r'); i != std::string::npos; i = body->find('\r', i)) {
        if (i + 1 < body->size() && (*body)[i + 1] == '\n') body->erase(i, 1);
        else (*body)[i] = '\n';
    }
    if (text_decide_codepage(*body, page) == TEXT_CODEPAGE_CP437)
        *body = text_cp437_to_utf8(*body);
    return true;
}

// Box drawing and block characters are what an ANSI drawing is made of. When the font
// in use has no glyphs for them the preview is correct and invisible, which looks from
// the outside exactly like the wrong code page - so say which of the two it is, and
// name the cure. Asked of the font's own coverage rather than matched against a list
// of fonts, because the answer depends on the file and the size the user chose.
static int qv_missing_art_glyphs(const char* text) {
    bool seen[0x25A1 - 0x2500] = {};
    int missing = 0;
    ImFont* font = ImGui::GetFont();
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    while (*p) {
        // Only U+2500..U+25A0 matters, and every one of them is three bytes. ASCII
        // cannot be in that range, so it is stepped over rather than decoded: this
        // runs on a preview that can be 64 KB, every frame.
        if (*p < 0x80) { p++; continue; }
        unsigned int cp;
        int extra;
        if ((*p & 0xE0) == 0xC0)      { cp = *p & 0x1Fu;  extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0Fu;  extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07u;  extra = 3; }
        else { p++; continue; }        // a stray byte: skip it, do not resync the world
        bool ok = true;
        for (int k = 1; k <= extra; k++) {
            if ((p[k] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (p[k] & 0x3Fu);
        }
        if (!ok) { p++; continue; }
        p += extra + 1;
        if (cp >= 0x2500 && cp <= 0x25A0) {
            int i = (int)cp - 0x2500;
            if (!seen[i]) {
                seen[i] = true;
                if (!font->IsGlyphInFont((ImWchar)cp)) missing++;
            }
        }
    }
    return missing;
}

// The line that tells the user their font is the problem rather than the file.
static void qv_font_note(const char* text) {
    const int missing = qv_missing_art_glyphs(text);
    if (missing <= 0) return;
    ImGui::TextDisabled("(%d of these characters have no glyph in this font: a font"
                        " with box drawing draws them - font = in the config, or"
                        " Options > Fonts and theme)", missing);
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

// The decoded pictures, kept apart from the text because the two hold things three
// orders of magnitude apart: 32 previews of a screenful of text is nothing, and four
// decoded photographs is a lot. Both bounds are applied where the picture is filed,
// not where it is drawn, and dropping one frees its texture with it - a cache that
// only ever grew would be holding GPU memory for files this session stopped looking
// at.
static QvImage* qv_image_find(AppPanes* app, const std::string& key) {
    for (std::pair<std::string, std::unique_ptr<QvImage>>& e : app->qv_images)
        if (e.first == key) return e.second.get();
    return nullptr;
}

static size_t qv_image_bytes(const AppPanes* app) {
    size_t total = 0;
    for (const std::pair<std::string, std::unique_ptr<QvImage>>& e : app->qv_images)
        total += e.second->rgba.size();
    return total;
}

static void qv_image_drop(AppPanes* app, size_t i) {
    tex_free(app->qv_images[i].second->tex);
    app->qv_images.erase(app->qv_images.begin() + (long)i);
}

static void qv_image_evict(AppPanes* app) {
    while (app->qv_images.size() > k_qv_image_cache_max) qv_image_drop(app, 0);
    // The byte bound may have to give way to the newest one on its own: a single
    // picture over the limit is kept rather than dropped the moment it arrives,
    // because the alternative is a preview that never appears at all.
    while (app->qv_images.size() > 1 && qv_image_bytes(app) > k_qv_image_cache_bytes)
        qv_image_drop(app, 0);
}

static void qv_image_put(AppPanes* app, const std::string& key, QvFetch* f) {
    for (size_t i = 0; i < app->qv_images.size(); i++)
        if (app->qv_images[i].first == key) { qv_image_drop(app, i); break; }
    std::unique_ptr<QvImage> img = std::make_unique<QvImage>();
    img->w          = f->img_w;
    img->h          = f->img_h;
    img->src_w      = f->img_src_w;
    img->src_h      = f->img_src_h;
    img->fmt        = f->img_fmt;
    img->orientation = f->img_orient;
    img->file_bytes = f->img_bytes;
    img->rgba       = std::move(f->img_rgba);
    app->qv_images.emplace_back(key, std::move(img));
    qv_image_evict(app);
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

// A name that says "picture" often enough to be worth reading whole on a thread
// instead of a screenful of it on the UI thread. This is only a routing hint: what a
// file is comes from its bytes, so a PNG named `data` is previewed too - it takes the
// other path, where the head read finds the magic and the rest goes to a worker.
static bool name_looks_like_image(const char* name) {
    static const char* ext[] = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".pbm",
                                ".pgm", ".ppm", ".pnm", ".tga", ".hdr", ".webp",
                                ".tif", ".tiff", ".svg", ".ico", nullptr};
    const char* dot = name ? strrchr(name, '.') : nullptr;
    if (!dot) return false;
    for (int i = 0; ext[i]; i++)
        if (!strcasecmp(dot, ext[i])) return true;
    return false;
}

// The whole of a file that turned out to be a picture, from the same open handle
// that found the magic: one read, decided by what the bytes are rather than by what
// the file is called.
static void qv_read_body(const std::string& path, std::string* body, ImgFormat* fmt,
                         std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { *err = "cannot read " + path; return; }
    char buf[8192];
    size_t n;
    while (body->size() < k_qv_max_bytes && (n = fread(buf, 1, sizeof buf, f)) > 0)
        body->append(buf, n);
    *fmt = img_sniff(*body);
    if (img_decodable(*fmt)) {
        while (body->size() < k_qv_image_max_bytes && (n = fread(buf, 1, sizeof buf, f)) > 0)
            body->append(buf, n);
    }
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
    ImgFormat fmt = IMG_NONE;
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
        if (err.empty()) {
            if (job.want_image) qv_read_body(file, &body, &fmt, &err);
            else qv_read_file(file, &body, &err);
        }
    }
    if (tmpdir[0]) fs_delete_entry(tmpdir);   // no shell: see close_viewer

    // A picture is settled here, on the worker: it is the only place the whole file
    // is in hand, and the only place a 12-megapixel decode is allowed to happen.
    // Anything the decoder refuses comes back as a sentence about why, which is what
    // a broken or oversized file deserves instead of a wall of hex.
    if (err.empty() && job.want_image) {
        ImgFormat kind = img_sniff(body);
        if (kind != IMG_NONE) {
            std::string why;
            int w = 0, h = 0, orient = 0;
            std::vector<unsigned char> rgba;
            // The canvas the file declares, asked for before the decode turns it:
            // a photograph stored sideways has two honest sizes and a caption that
            // shows only one of them is half the story.
            int src_w = 0, src_h = 0;
            std::string dwhy;
            if (img_dims(body, &src_w, &src_h, &dwhy)) { /* the header's own numbers */ }
            if (img_decode(body, kind, &w, &h, &rgba, &orient, &why)) {
                int tw = w, th = h;
                img_fit(&tw, &th, &rgba, k_qv_tex_max_px);
                f->is_image   = true;
                f->img_fmt    = kind;
                f->img_src_w  = src_w ? src_w : w;
                f->img_src_h  = src_h ? src_h : h;
                f->img_w      = tw;
                f->img_h      = th;
                f->img_orient = orient;
                f->img_bytes  = (long long)body.size();
                f->img_rgba   = std::move(rgba);
            } else {
                f->text = "(" + why + ")";
            }
            f->done = true;
            return;
        }
    }

    if (err.empty() && !qv_to_text(&body, job.codepage)) body = k_qv_binary;
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
        if (f->err.empty()) {
            if (f->is_image) qv_image_put(app, f->key, f);
            else qv_cache_put(app, f->key, f->text);
        }
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
    // Whether this row may turn out to be a picture, and may therefore be read
    // further than a screenful. A site is left out on purpose: a preview is not a
    // reason to pull a whole photograph down just to find out what a row is, and the
    // byte cap that keeps a preview cheap would have to go with it. A member of an
    // archive is read whole when it is a picture because it is already local by the
    // time it is looked at; a name on this disk only routes the read onto a thread,
    // and a picture with no extension in its name is still found, one frame later.
    job->want_image = app->qv_images_on && job->kind != QV_REMOTE &&
                      (job->kind == QV_ARCHIVE ||
                       name_looks_like_image(pane.entries[idx]->name));
    job->codepage = text_codepage_from_name(app->preview_codepage);
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
    // A picture is drawn rather than printed, so there is no text to hand back for
    // one. The window that can draw it asks through qv_draw_image().
    if (app->qv_images_on && qv_image_find(app, job.key)) return "";

    if (job.kind == QV_LOCAL && !job.want_image) {
        std::string body, err;
        qv_read_file(job.path, &body, &err);
        if (!err.empty()) return "(" + err + ")";
        if (app->qv_images_on && img_sniff(body) != IMG_NONE) {
            // A picture whose name did not say picture, so it was read like any
            // other file and only the head of it is in hand. Reading the rest of a
            // 3 MB file here would stall the frame, which is the one thing all of
            // this is arranged to avoid, so the rest goes to a worker and this frame
            // says what it is doing.
            QvJob full = job;
            full.want_image = true;
            for (std::unique_ptr<QvFetch>& f : app->qv_fetches)
                if (f->key == full.key) { f->wanted = true; return k_qv_reading; }
            if (app->qv_fetches.size() < k_qv_inflight_max) qv_start(app, full);
            return k_qv_reading;
        }
        if (!qv_to_text(&body, job.codepage)) body = k_qv_binary;
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

// A decoded picture, drawn. True when this row is a picture and it was put on the
// screen, false when there is nothing drawn yet - which is the signal for the caller
// to fall back on the text path, including the hex dump a binary file has always
// gotten. One function for both quick views, so the F3 window and the Ctrl+Q panel
// cannot drift apart on what a preview of a picture looks like.
//
// The texture is made here rather than when the pixels arrive, because asking for a
// preview and then arrowing past is the common case and should not have cost a GL
// texture. It is kept afterwards, because the window is resized with the mouse and a
// picture has to refit every frame.
static bool qv_draw_image(AppPanes* app, const Pane& pane, int idx) {
    if (!app->qv_images_on) return false;
    if (idx < 0 || idx >= pane.entry_count || !pane.entries) return false;
    if (is_directory(pane.entries[idx]->name, pane.entries[idx]->mode)) return false;
    QvImage* img = qv_image_find(app, pane_row_path(pane, idx));
    if (!img) return false;   // qv_preview() is the half that starts the read

    if (!img->tex) img->tex = tex_upload(img->w, img->h, img->rgba.data());
    if (!img->tex) {
        // Decoded, and nowhere to put it: no GL context, or a build whose renderer
        // never loaded the entry points. Better to say that than to draw nothing.
        ImGui::TextUnformatted("(no picture on screen to draw this in)");
        return true;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale = 1.0f;
    if (img->w > 0 && img->h > 0 && avail.x > 1.0f && avail.y > 1.0f) {
        scale = avail.x / (float)img->w;
        if (avail.y / (float)img->h < scale) scale = avail.y / (float)img->h;
        if (scale > 1.0f) scale = 1.0f;   // never enlarge: a thumbnail is not a preview
    }
    const float dw = (float)img->w * scale, dh = (float)img->h * scale;

    ImGui::PushStyleVar(ImGuiStyleVar_ImageBorderSize, 0.0f);
    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)img->tex), ImVec2(dw, dh));
    ImGui::PopStyleVar();

    // What the file is, and what is being shown, because "4032x3024 shown at
    // 1365x1024" is the difference between a preview and a lie about one.
    char cap[256];
    snprintf(cap, sizeof cap, "%dx%d %s  shown %dx%d%s",
             img->src_w, img->src_h, img_format_name(img->fmt), img->w, img->h,
             img->orientation >= 2 ? "  turned to match the camera" : "");
    ImGui::TextUnformatted(cap);
    return true;
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
    if (qv_draw_image(app, src, src.selected)) {
        ImGui::TextDisabled("%s", e->name);
        ImGui::EndChild();
        return;
    }
    std::string text = qv_preview(app, src, src.selected);
    ImGui::TextDisabled("%s", e->name);
    ImGui::Separator();
    ImGui::TextUnformatted(text.empty() ? "(nothing to preview)" : text.c_str());
    qv_font_note(text.c_str());
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
// A small icon of the app's own, for when the shell has none to give: every row on
// Linux, ".." everywhere, and anything the shell does not answer for. Drawn rather
// than shipped as pictures, so it is sharp at any font size.
static void draw_kind_icon(ImDrawList* dl, IconKind kind, ImVec2 p, float s) {
    auto at = [&](float x, float y) { return ImVec2(p.x + x * s, p.y + y * s); };
    const ImU32 outline = IM_COL32(110, 110, 110, 255);
    const ImU32 folder = IM_COL32(240, 196, 80, 255);
    const ImU32 folder_tab = IM_COL32(214, 164, 54, 255);
    const float r = s * 0.08f;

    auto page = [&](ImU32 fill) {
        ImVec2 pts[5] = { at(0.20f, 0.06f), at(0.60f, 0.06f), at(0.82f, 0.28f),
                          at(0.82f, 0.94f), at(0.20f, 0.94f) };
        dl->AddConvexPolyFilled(pts, 5, fill);
        dl->AddPolyline(pts, 5, outline, ImDrawFlags_Closed, 1.0f);
        ImVec2 fold[3] = { at(0.60f, 0.06f), at(0.60f, 0.28f), at(0.82f, 0.28f) };
        dl->AddPolyline(fold, 3, outline, 0, 1.0f);
    };
    auto folder_shape = [&]() {
        dl->AddRectFilled(at(0.06f, 0.14f), at(0.46f, 0.32f), folder_tab, r);
        dl->AddRectFilled(at(0.06f, 0.26f), at(0.94f, 0.86f), folder, r);
    };

    switch (kind) {
        case ICON_UP: {
            const ImU32 c = ImGui::GetColorU32(mc_theme_colors()->dir);
            dl->AddTriangleFilled(at(0.50f, 0.10f), at(0.16f, 0.50f), at(0.84f, 0.50f), c);
            dl->AddRectFilled(at(0.38f, 0.48f), at(0.62f, 0.88f), c);
            break;
        }
        case ICON_DIR:
            folder_shape();
            break;
        case ICON_ARCHIVE:
            // A folder with a zip down the middle, the way TC and Explorer draw one.
            folder_shape();
            for (int k = 0; k < 4; k++) {
                float y = 0.30f + k * 0.13f;
                dl->AddRectFilled(at(k % 2 ? 0.50f : 0.42f, y), at(k % 2 ? 0.58f : 0.50f, y + 0.10f),
                                  IM_COL32(90, 70, 40, 255));
            }
            break;
        case ICON_EXEC:
            // A window: a frame with a title bar.
            dl->AddRectFilled(at(0.08f, 0.16f), at(0.92f, 0.84f), IM_COL32(236, 236, 236, 255));
            dl->AddRectFilled(at(0.08f, 0.16f), at(0.92f, 0.34f), IM_COL32(60, 120, 210, 255));
            dl->AddRect(at(0.08f, 0.16f), at(0.92f, 0.84f), outline, 0.0f, 0, 1.0f);
            break;
        case ICON_IMAGE:
            page(IM_COL32(245, 245, 245, 255));
            dl->AddTriangleFilled(at(0.26f, 0.86f), at(0.44f, 0.56f), at(0.62f, 0.86f), IM_COL32(70, 160, 80, 255));
            dl->AddTriangleFilled(at(0.48f, 0.86f), at(0.62f, 0.66f), at(0.76f, 0.86f), IM_COL32(50, 130, 70, 255));
            dl->AddCircleFilled(at(0.66f, 0.44f), s * 0.07f, IM_COL32(235, 170, 40, 255));
            break;
        case ICON_FILE:
        default:
            page(IM_COL32(245, 245, 245, 255));
            break;
    }
}

// The icon in front of a row's name (Options > Show file icons), at the cursor:
// the shell's own where there is one, the drawn one otherwise. Returns how far the
// name has to move right to make room for it - 0 with icons off. A row scrolled out
// of sight is not asked about: a file's own icon is a disk read, and the few the
// shell is allowed per frame should go to the rows someone can see.
static float draw_row_icon(const AppPanes* app, const Pane& pane, int i, bool is_dir, bool is_exec) {
    if (!app->show_icons) return 0.0f;
    const float s = ImGui::GetTextLineHeight();
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.x = floorf(p.x);
    p.y = floorf(p.y);
    if (!ImGui::IsRectVisible(p, ImVec2(p.x + s, p.y + s))) return s + gap;
    const FileEntry* e = pane.entries[i];
    const bool local = !in_remote(pane) && !in_archive(pane);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    unsigned long long tex = icon_shell(pane.current_dir, entry_rel(pane, i), is_dir, local);
    if (tex)
        dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)tex), p, ImVec2(p.x + s, p.y + s));
    else
        draw_kind_icon(dl, icon_kind(e->name, is_dir, is_exec), p, s);
    return s + gap;
}

// Sort both panes by `mode` (an FsSortMode). Asking for the key already in use
// flips its direction, as a second click on a Total Commander column header or a
// second Ctrl+F5 does. A new key starts the way it is usually wanted: names and
// extensions A to Z, dates newest first, sizes biggest first.
static void set_sort(AppPanes* app, int mode) {
    static const char* names[5] = { "name", "ext", "size", "date", "unsorted" };
    if (mode == fs_get_sort_mode() && mode != FS_SORT_NONE) {
        fs_set_sort_desc(!fs_get_sort_desc());
    } else {
        fs_set_sort_mode(mode);
        fs_set_sort_desc(mode == FS_SORT_DATE || mode == FS_SORT_SIZE);
    }
    if (mode == FS_SORT_NONE)
        snprintf(app->status_msg, sizeof(app->status_msg), "Sort by: unsorted");
    else
        snprintf(app->status_msg, sizeof(app->status_msg), "Sort by: %s, %s", names[mode],
                 fs_get_sort_desc() ? "descending" : "ascending");
    app_panes_refresh_left(app);
    app_panes_refresh_right(app);
}

// The sort key a long-view column header stands for, -1 for one that sorts nothing.
static int col_sort_key(int col) {
    switch (col) {
        case COL_NAME: return FS_SORT_NAME;
        case COL_EXT:  return FS_SORT_EXT;
        case COL_SIZE: return FS_SORT_SIZE;
        case COL_DATE: return FS_SORT_DATE;
        default:       return -1;
    }
}

// The header row of the long and mid views: one button per column. A click sorts
// by that column, a second click reverses it, and the column the panes are sorted
// by carries a small arrow pointing the way the order runs.
static void render_sort_headers(AppPanes* app, const int* col_order, int ncols) {
    // The file rows blank the Header colors so hovering a row does not paint it;
    // the header buttons want the real ones back to show they can be pressed.
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Header, st.Colors[ImGuiCol_Header]);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, st.Colors[ImGuiCol_HeaderHovered]);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, st.Colors[ImGuiCol_HeaderActive]);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int k = 0; k < ncols; k++) {
        ImGui::TableSetColumnIndex(k);
        int key = col_sort_key(col_order[k]);
        ImGui::PushID(k);
        ImGui::TableHeader(k_col_label[col_order[k]]);   // a button with no return value
        if (key >= 0 && ImGui::IsItemClicked(ImGuiMouseButton_Left)) set_sort(app, key);
        if (key >= 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip(key == fs_get_sort_mode() ? "Click to reverse the order"
                                                        : "Click to sort by this column");
        if (key >= 0 && key == fs_get_sort_mode()) {
            ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            float h = ImGui::GetFontSize() * 0.35f;
            float cx = mx.x - h - st.CellPadding.x, cy = (mn.y + mx.y) * 0.5f;
            ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (fs_get_sort_desc())
                dl->AddTriangleFilled(ImVec2(cx - h, cy - h * 0.5f), ImVec2(cx + h, cy - h * 0.5f),
                                      ImVec2(cx, cy + h * 0.6f), col);
            else
                dl->AddTriangleFilled(ImVec2(cx - h, cy + h * 0.5f), ImVec2(cx, cy - h * 0.6f),
                                      ImVec2(cx + h, cy + h * 0.5f), col);
        }
        ImGui::PopID();
    }
    ImGui::PopStyleColor(3);
}

// Drag-and-drop payloads: a local file or folder picked up from a pane row (its
// full path, NUL-terminated), and a launch bar button being moved (its index).
static const char* const k_dnd_path   = "IMMC_PATH";
static const char* const k_dnd_launch = "IMMC_LAUNCH";

// Lets the row just drawn be dragged, onto the launch bar for now. Only a local
// one: a file on a site or in an archive is not something a button can start.
static void pane_row_drag_source(const Pane& pane, int i) {
    if (in_remote(pane) || in_archive(pane) || !strcmp(pane.entries[i]->name, "..")) return;
    if (!ImGui::BeginDragDropSource()) return;
    char full[4300];
    snprintf(full, sizeof(full), "%s/%s", pane.current_dir, entry_rel(pane, i));
    ImGui::SetDragDropPayload(k_dnd_path, full, strlen(full) + 1);
    ImGui::Text("%s", entry_rel(pane, i));
    ImGui::TextDisabled("drop on the launch bar to add a button");
    ImGui::EndDragDropSource();
}

static void render_file_list_body(AppPanes* app, Pane& pane, bool focused) {
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    pane.page_size = row_h > 0 ? (int)(ImGui::GetWindowSize().y / row_h) : PAGE_SIZE;
    if (pane.page_size < 1) pane.page_size = PAGE_SIZE;

    // Clicking anywhere in the pane - below the last row, between rows, on the
    // scrollbar - makes it the active pane. Checked on the child window, because
    // IsItemHovered() would refer to the widget before it; the long view's table
    // is a window of its own inside it, hence ChildWindows.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(0))
        app->left_focus = (&pane == &app->left);
    const ImGuiID ctx_id = ImGui::GetID("ctxmenu");

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
        // The table scrolls itself (ScrollY), so its header row can stay frozen at
        // the top while the rows move under it. It fills the child, leaving a line
        // for the "no entries match" note when the filter hides everything.
        float table_h = pane.view.empty() ? -row_h : 0.0f;
        if (ImGui::BeginTable("##filetable", ncols,
                              ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_ScrollY, ImVec2(0.0f, table_h))) {
            for (int k = 0; k < ncols; k++) {
                int c = col_order[k];
                if (c == COL_NAME)
                    ImGui::TableSetupColumn(k_col_label[c], ImGuiTableColumnFlags_WidthStretch, 0.42f);
                else
                    ImGui::TableSetupColumn(k_col_label[c], ImGuiTableColumnFlags_WidthFixed, k_col_width[c]);
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            // A page is what fits under the header in the table's own window.
            if (row_h > 0) {
                pane.page_size = (int)(ImGui::GetWindowSize().y / row_h) - 1;
                if (pane.page_size < 1) pane.page_size = 1;
            }
            render_sort_headers(app, col_order, ncols);

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

                if (float iw = draw_row_icon(app, pane, i, is_dir, is_exec))
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + iw);
                bool clicked = ImGui::Selectable(entry_rel(pane, i), selected, ImGuiSelectableFlags_SpanAllColumns);
                pane_row_drag_source(pane, i);
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
                    // By the id taken outside the table: in here the table's own
                    // window is current, and "ctxmenu" would hash to a popup the
                    // BeginPopup below never looks for.
                    ImGui::OpenPopup(ctx_id);
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

            if (float iw = draw_row_icon(app, pane, i, is_dir, is_exec))
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + iw);
            bool clicked = ImGui::Selectable(entry_rel(pane, i), selected);
            pane_row_drag_source(pane, i);
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
        if (app->popup_close_request) ImGui::CloseCurrentPopup();
        else app->pane_menu_alive = true;
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
        if (app->popup_close_request) ImGui::CloseCurrentPopup();
        else app->ctx_menu_alive = true;
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
    ImGui::SetNextWindowSize(ImVec2(app->qv_w, app->qv_h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 640, 60), ImGuiCond_FirstUseEver);
    ImGui::Begin("Quick view");
    // Remember the shape to open at next session. FirstUseEver means ImGui holds
    // the live size for the rest of this one, so what gets recorded here is what
    // the user dragged the window to, not what it was asked to be. With F3 on both
    // panes there are two such windows and the last one drawn is the one whose
    // size is kept; both open at that size.
    const ImVec2 qv_size = ImGui::GetWindowSize();
    app->qv_w = qv_size.x;
    app->qv_h = qv_size.y;

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
        // A picture first. Only when there is no picture to show does this row go
        // through the text path, so a photograph never gets a hex dump.
        if (qv_draw_image(app, pane, pane.selected)) {
            // drawn, with its caption
        } else {
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
                qv_font_note(text.c_str());
            }
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
    qv_font_note(viewer_buf);
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

// Alt+F8: the commands again, newest first, inside the console window. Picking one
// puts it in the field rather than running it: a list of commands that runs what
// you clicked is a list you have to read carefully before touching.
static void render_console_hist(AppPanes* app) {
    if (app->console_hist.empty()) {
        ImGui::TextDisabled("(no commands yet)");
        return;
    }
    for (int i = (int)app->console_hist.size() - 1; i >= 0; i--) {
        ImGui::PushID(i);
        if (ImGui::Selectable(app->console_hist[i].c_str(), false)) {
            console_set_input(app, app->console_hist[i].c_str());
            app->console_hist_pos = (int)app->console_hist.size();
            app->console_hist_open = false;
            app->console_focus_request = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
}

static void netlog_time(time_t when, char* out, size_t n) {
    struct tm tmv;
    localtime_r(&when, &tmv);
    strftime(out, n, "%H:%M:%S", &tmv);
}

// Lines that belong to one server reply: replies, one after the other, from the
// same connection. curl hands a multi-line reply over a line at a time, and a
// welcome banner drawn in box characters is exactly that.
static bool netlog_same_reply(const NetLogLine& a, const NetLogLine& b) {
    return a.kind == NETLOG_RECV && b.kind == NETLOG_RECV &&
           a.thread == b.thread && a.site == b.site;
}

// What each log line looks like on screen, from `from` on. A server reply is read
// the way the quick view reads an .nfo: a banner from a DOS-era scene server is
// CP437 box drawing, which is not UTF-8 and would draw as nothing, so it goes
// through text.h's rule and `preview_codepage` like any other file. The rule needs
// a few art bytes to decide, more than one line of a border may have, so it is
// asked of the whole reply - and a reply that grew since the last frame is
// decided again from its first line.
static void netlog_restyle(const std::vector<NetLogLine>& lines, std::vector<std::string>* shown,
                           size_t from, TextCodepage page) {
    shown->resize(lines.size());
    while (from > 0 && from < lines.size() && netlog_same_reply(lines[from - 1], lines[from])) from--;
    size_t i = from;
    while (i < lines.size()) {
        size_t j = i + 1;
        while (j < lines.size() && netlog_same_reply(lines[j - 1], lines[j])) j++;
        bool cp437 = false;
        if (lines[i].kind == NETLOG_RECV) {
            std::string reply;
            for (size_t k = i; k < j; k++) reply += lines[k].text + "\n";
            cp437 = text_decide_codepage(reply, page) == TEXT_CODEPAGE_CP437;
        }
        for (size_t k = i; k < j; k++)
            (*shown)[k] = cp437 ? text_cp437_to_utf8(lines[k].text) : lines[k].text;
        i = j;
    }
}

// The connection log: every line remote.cpp recorded about talking to a server,
// newest at the bottom. It opens with a connect, small and in the corner, so a
// connect that takes its time shows what it is waiting on - a lookup, a TCP
// connect, a TLS handshake, a reply to PASV - while the panes stay usable.
static void render_netlog(AppPanes* app) {
    if (!app->netlog_open) return;

    // Copy out only what is new since the last frame; the log keeps its own cap,
    // and so does this copy of it.
    size_t had = app->netlog.size();
    app->netlog_seq = net_log_since(app->netlog_seq, &app->netlog);
    app->netlog_shown.resize(app->netlog.size());
    const size_t keep = 4000;
    if (app->netlog.size() > keep) {
        const size_t drop = app->netlog.size() - keep;
        app->netlog.erase(app->netlog.begin(), app->netlog.begin() + (long)drop);
        app->netlog_shown.erase(app->netlog_shown.begin(), app->netlog_shown.begin() + (long)drop);
        had = had > drop ? had - drop : 0;
    }
    // A change of `preview_codepage` in Options redraws everything in the new one.
    const TextCodepage page = text_codepage_from_name(app->preview_codepage);
    if (page != app->netlog_page) { app->netlog_page = page; had = 0; }
    if (had < app->netlog.size()) netlog_restyle(app->netlog, &app->netlog_shown, had, page);

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const ImVec2 size(ImMin(640.0f, disp.x - 24.0f), ImMin(220.0f, disp.y * 0.4f));
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(disp.x - size.x - 12.0f, disp.y - size.y - 60.0f),
                            ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Connection log", &open, ImGuiWindowFlags_NoCollapse);

    if (ImGui::SmallButton("Clear")) {
        net_log_clear();
        app->netlog.clear();
        app->netlog_shown.clear();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        std::string all;
        for (size_t i = 0; i < app->netlog.size(); i++) {
            const NetLogLine& l = app->netlog[i];
            char ts[16];
            netlog_time(l.when, ts, sizeof ts);
            const char* mark = l.kind == NETLOG_SENT ? "> " : l.kind == NETLOG_RECV ? "< "
                             : l.kind == NETLOG_ERROR ? "! " : "* ";
            all += std::string(ts) + " [" + std::to_string(l.thread) + "] " + mark +
                   app->netlog_shown[i] + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &app->netlog_follow);
    ImGui::Separator();

    ImGui::BeginChild("##netlog_lines", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    if (app->netlog.empty())
        ImGui::TextDisabled("(nothing yet - connect to a site and its conversation shows up here)");
    // A long session is thousands of lines; only the visible ones are drawn. The
    // prefix is one width on every line, so a banner's columns stay lined up.
    ImGuiListClipper clip;
    clip.Begin((int)app->netlog.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const NetLogLine& l = app->netlog[(size_t)i];
            const char* text = app->netlog_shown[(size_t)i].c_str();
            char ts[16];
            netlog_time(l.when, ts, sizeof ts);
            ImGui::TextDisabled("%s %2d", ts, l.thread);
            ImGui::SameLine();
            switch (l.kind) {
                case NETLOG_SENT:  ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "> %s", text); break;
                case NETLOG_RECV:  ImGui::TextColored(ImVec4(0.55f, 0.90f, 0.55f, 1.0f), "< %s", text); break;
                case NETLOG_ERROR: ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "! %s", text); break;
                default:           ImGui::TextDisabled("* %s", text); break;
            }
        }
    }
    // Scrolling up to read stops the follow; scrolling back to the bottom resumes it.
    if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1.0f && ImGui::IsWindowHovered() &&
        ImGui::GetIO().MouseWheel > 0.0f)
        app->netlog_follow = false;
    if (app->netlog_follow) ImGui::SetScrollHereY(1.0f);
    else if (ImGui::GetScrollMaxY() > 0.0f && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        app->netlog_follow = true;
    ImGui::EndChild();

    ImGui::End();
    if (!open) app->netlog_open = false;
}

// The console window: the backlog above, the line being typed below, and the
// directory the next command will run in. It is a window over the panes, not a
// modal - it holds the keyboard only while its field holds it, and Esc hides it
// without touching the backlog, which is the whole point of the feature: the
// transcript is still there when it comes back, in another directory or after a
// restart.

static void render_console(AppPanes* app) {
    if (!app->console_open) return;
    Pane& active = app->left_focus ? app->left : app->right;

    ImGui::SetNextWindowSize(ImVec2(app->console_w, app->console_h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 380.0f,
                                   ImGui::GetIO().DisplaySize.y * 0.5f - 210.0f),
                            ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Console (Ctrl+Enter)", &open, ImGuiWindowFlags_NoCollapse);
    // Remember the shape to open at next session, as the quick view does.
    const ImVec2 csz = ImGui::GetWindowSize();
    app->console_w = csz.x;
    app->console_h = csz.y;

    // Where the next command runs, said out loud, because it is the one thing about
    // a console in a file manager that is not obvious: commands run in the directory
    // the focused pane is showing, not the one the app was started in.
    ImGui::TextDisabled("in %s", active.current_dir);
    if (app->console_job) {
        ImGui::SameLine();
        ImGui::TextDisabled("- running, Esc cancels");
    }
    ImGui::Separator();

    // The transcript, with the input line left a row of space at the bottom of the
    // window by asking the child for everything except that row.
    const float line_h = ImGui::GetTextLineHeightWithSpacing() +
                         ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::BeginChild("##console_log", ImVec2(0.0f, -line_h), true);
    if (app->console_hist_open) {
        render_console_hist(app);
    } else {
        if (app->console_log.empty())
            ImGui::TextDisabled("(nothing yet - type a command and press Enter)");
        for (size_t i = 0; i < app->console_log.size(); i++) {
            const ConsoleEntry& e = app->console_log[i];
            ImGui::PushID((int)i);
            // The directory, but only where it changes. A backlog that spans three
            // directories has to say which command ran where, and repeating the same
            // path above every command says it so often it stops being read.
            if (i == 0 || app->console_log[i - 1].cwd != e.cwd)
                ImGui::TextDisabled("# %s", e.cwd.c_str());
            ImGui::TextDisabled("$ %s", e.cmd.c_str());
            if (!e.out.empty()) ImGui::TextUnformatted(e.out.c_str());
            ImGui::Spacing();
            ImGui::PopID();
        }
        // The command still running, whose output arrives while it goes. That is
        // what the worker thread is for: a `find` that takes a minute shows its
        // results during the minute, and the panes keep answering in the meantime.
        if (app->console_job) {
            std::string live;
            { std::lock_guard<std::mutex> lk(app->console_job->mu); live = app->console_job->out; }
            if (!live.empty()) ImGui::TextUnformatted(live.c_str());
        }
    }
    // Follow the tail of a running command unless the user scrolled up to read
    // something older: without the lock the line being typed gets pushed off screen
    // by every chunk of output.
    const float max_scroll = ImGui::GetScrollMaxY();
    app->console_scroll_lock = (max_scroll > 2.0f && ImGui::GetScrollY() < max_scroll - 2.0f);
    if (!app->console_scroll_lock) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (app->console_focus_request) {
        ImGui::SetKeyboardFocusHere();
        app->console_focus_request = false;
    }
    // A line the app wrote (Enter cleared it, Up recalled one, Ctrl+P appended a
    // path) has to be handed to the field explicitly while the field owns the
    // buffer, or the user keeps seeing the text that was there before.
    if (app->console_input_reload) {
        app->console_input_reload = false;
        if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetID("##console")))
            st->ReloadUserBufAndMoveToEnd();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##console", app->console_input, sizeof(app->console_input));
    // The claim is what decides who owns the keyboard: a click onto a pane row hands
    // it back to the panes and the window stays as a transcript that is still readable.
    app->console_field_active = ImGui::IsItemActive();

    if (!open) app->console_open = false;
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

// Alt+F8 lives inside the console window now (render_console_hist), so there is no
// history window of its own to draw.

#ifdef _WIN32
// Drives for the Alt+F1/F2 picker: every drive letter, from platform.cpp.
static void list_mounts(StringList* mnts, StringList* labels) {
    plat_list_drives(mnts, labels);
}
#else
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
#endif

// The picker's rows: the drives, then the favorites (the hotlist). One list to the
// arrow keys and to Enter; `drives` says where the favorites begin.
static void volumes_rows(const AppPanes* app, StringList* paths, StringList* labels, int* drives) {
    list_mounts(paths, labels);
    *drives = (int)paths->size();
    for (const std::string& f : app->hotlist) {
        paths->push_back(f);
        labels->push_back(f);
    }
}

// Take favorite row `row` out of the hotlist, keeping the cursor on the list.
static void volumes_remove_favorite(AppPanes* app, int row, int drives) {
    int f = row - drives;
    if (f < 0 || f >= (int)app->hotlist.size()) return;
    snprintf(app->status_msg, sizeof(app->status_msg), "Removed from favorites: %s",
             app->hotlist[f].c_str());
    app->hotlist.erase(app->hotlist.begin() + f);
    hotlist_save(app);
    int count = drives + (int)app->hotlist.size();
    if (app->volumes_cursor >= count) app->volumes_cursor = count - 1;
    app->volumes_moved = true;
}

// Open drive `i` of the list in the pane the picker is for, and close the picker.
// The mouse and Enter both come here, so they cannot disagree about what a pick does.
static void volumes_pick(AppPanes* app, const StringList& mnt, int i) {
    if (i < 0 || i >= (int)mnt.size()) return;
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
    app->volumes_cursor = -1;
}

// Where the cursor starts: on the drive the pane is on now - the mount whose path is
// the longest prefix of the pane's directory - so Enter alone changes nothing and
// one arrow press is the drive next to it.
static int volumes_home_row(const AppPanes* app, const StringList& mnt) {
    const Pane& pane = app->volumes_target == 0 ? app->left : app->right;
    int best = mnt.empty() ? -1 : 0;
    size_t best_len = 0;
    for (int i = 0; i < (int)mnt.size(); i++) {
        const std::string& m = mnt[i];
        if (m.size() <= best_len) continue;
#ifdef _WIN32
        bool prefix = !strncasecmp(pane.current_dir, m.c_str(), m.size());
#else
        bool prefix = !strncmp(pane.current_dir, m.c_str(), m.size()) &&
                      (m == "/" || pane.current_dir[m.size()] == '/' ||
                       pane.current_dir[m.size()] == '\0');
#endif
        if (prefix) { best = i; best_len = m.size(); }
    }
    return best;
}

// The row a typed letter means: the next drive after the cursor whose name starts
// with it - "D" is D:/ on Windows, and on Linux the first mount whose last part does
// (/media/data for "d"). -1 when none does.
static int volumes_letter_row(const StringList& mnt, int cur, char letter) {
    int n = (int)mnt.size();
    for (int step = 1; step <= n; step++) {
        int i = ((cur < 0 ? -1 : cur) + step) % n;
        const std::string& m = mnt[i];
        size_t slash = m.find_last_of('/', m.size() > 1 ? m.size() - 2 : 0);
        char first = m.size() > 1 && m[1] == ':' ? m[0]
                   : (slash != std::string::npos && slash + 1 < m.size() ? m[slash + 1] : m[0]);
        if (tolower((unsigned char)first) == tolower((unsigned char)letter)) return i;
    }
    return -1;
}

// Alt+F1 / Alt+F2: choose the drive shown in the left/right pane.
static void render_volumes(AppPanes* app) {
    if (!app->volumes_open) return;
    StringList mnt, label;
    int drives = 0;
    volumes_rows(app, &mnt, &label, &drives);
    if (app->volumes_cursor < 0 || app->volumes_cursor >= (int)mnt.size()) {
        app->volumes_cursor = volumes_home_row(app, StringList(mnt.begin(), mnt.begin() + drives));
        app->volumes_moved = true;
    }
    ImGui::SetNextWindowSize(ImVec2(640, 320), ImGuiCond_FirstUseEver);
    bool open = true;
    char title[80];
    snprintf(title, sizeof(title), "Drives for the %s pane (Alt+F1/F2)",
             app->volumes_target == 0 ? "left" : "right");
    ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse);
    if (drives == 0) ImGui::TextUnformatted("(no mounted drives found)");
    else ImGui::TextDisabled("arrows and Enter, or the drive's letter; Esc closes");
    int remove_row = -1;
    for (int i = 0; i < (int)mnt.size(); i++) {
        if (i == drives) {
            ImGui::Separator();
            ImGui::TextDisabled("Favorites   Ins or Alt+D adds the pane's folder, Del removes one");
        }
        ImGui::PushID(i);
        bool fav = i >= drives;
        // A favorite's row stops where its button begins, or the row would take the
        // button's click.
        float w = fav ? ImGui::GetContentRegionAvail().x - 70.0f : 0.0f;
        bool clicked = ImGui::Selectable(label[i].c_str(), app->volumes_cursor == i, 0,
                                         ImVec2(w > 40.0f || !fav ? w : 40.0f, 0));
        if (app->volumes_moved && app->volumes_cursor == i) ImGui::SetScrollHereY(0.5f);
        if (fav) {
            ImGui::SameLine();
            if (ImGui::SmallButton("remove")) remove_row = i;
        }
        ImGui::PopID();
        if (clicked) { volumes_pick(app, mnt, i); break; }
    }
    if (drives > 0 && app->hotlist.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("No favorites yet: Ins here, or Alt+D in a pane, adds the folder");
    }
    if (remove_row >= 0) volumes_remove_favorite(app, remove_row, drives);
    app->volumes_moved = false;
    if (!open) { app->volumes_open = false; app->volumes_cursor = -1; }
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

// A question with three answers, for the ones Y/N cannot hold. Continuing a
// stopped transfer, throwing away what it already earned, and leaving the row
// alone are three different intentions, and the middle one never gets noticed
// unless it is spelled out next to the other two.
static void render_choice(AppPanes* app) {
    if (app->choice == CHOICE_NONE) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(560, 140), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 280, io.DisplaySize.y * 0.5f - 70), ImGuiCond_Always);
    ImGui::Begin("Resume transfer", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
    ImGui::TextWrapped("%s", app->choice_text);
    ImGui::Spacing();
    // The partial is the useful default and the one that needs no explanation, so
    // it is first and it is what Enter answers.
    if (ImGui::Button("Resume")) app->choice_pick = 0;
    ImGui::SameLine();
    if (ImGui::Button("Start over")) app->choice_pick = 1;
    ImGui::SameLine();
    if (ImGui::Button("Skip")) app->choice_pick = 2;
    ImGui::SameLine();
    ImGui::TextUnformatted("(press R / S / Esc)");
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
    const bool overwrite_confirm = confirm_is_overwrite(app->confirm);
    if (app->confirm == CONF_SFV_VERIFY) {
        if (ImGui::Button("Verify")) app->pending_confirm_answer = CONF_ANS_VERIFY;
        ImGui::SameLine();
        if (ImGui::Button("Not now")) app->pending_confirm_answer = CONF_ANS_NOT_NOW;
        ImGui::SameLine();
        if (ImGui::Button("Always")) app->pending_confirm_answer = CONF_ANS_ALWAYS;
        ImGui::SameLine();
        ImGui::TextUnformatted("(press V / N / A)");
        ImGui::End();
        return;
    }
    // An archive extraction gets two answers and not three. The names inside an
    // archive are the extractor's to write, and it writes them where the archive
    // says they go: "write beside" is not something it can be asked for a subtree,
    // so the dialog does not offer a promise the tool cannot keep.
    const bool rename_too = overwrite_confirm && app->confirm != CONF_OVERWRITE_EXTRACT;
    if (overwrite_confirm) {
        if (ImGui::Button("Overwrite")) app->pending_confirm_answer = CONF_ANS_OVERWRITE;
        ImGui::SameLine();
        if (ImGui::Button("Skip")) app->pending_confirm_answer = CONF_ANS_SKIP;
        if (rename_too) {
            ImGui::SameLine();
            if (ImGui::Button("Rename")) app->pending_confirm_answer = CONF_ANS_RENAME;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(rename_too ? "(press Y / N / R - Esc cancels the operation)"
                                          : "(press Y / N - Esc cancels the operation)");
    } else {
        if (ImGui::Button("Yes")) app->pending_confirm_answer = CONF_ANS_YES;
        ImGui::SameLine();
        if (ImGui::Button("No")) app->pending_confirm_answer = CONF_ANS_NO;
        ImGui::SameLine();
        ImGui::TextUnformatted("(press Y / N)");
    }
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
        "F3 draws a picture as a picture; one this build cannot decode says which kind it is\n"
        "F3 reads an old .nfo's box drawing as CP437 when the bytes look like a drawing\n"
        "Tab switch pane    Enter open/run   Backspace or Ctrl+PgUp parent\n"
        "Ctrl+PgDn open entry under cursor  Alt+Left/Right dir history\n"
        "Ctrl+T new tab     Ctrl+W close     Ctrl+Shift+W close all tabs\n"
        "Ctrl+Tab / Ctrl+Shift+Tab cycle    Ctrl+1..9 jump to a tab\n"
        "Ctrl+Up / Ctrl+Shift+Down open the cursor dir in a new tab\n"
        "Ctrl+Left/Right show the cursor dir in the other pane\n"
        "Ctrl+U swap the pane directories   Ctrl+, go to the root\n"
        "Ctrl+D leave the site this pane is on, and close its connection\n"
        "Ctrl+C copy        Ctrl+X cut       Ctrl+V paste   Ctrl+P path into the console\n"
        "Alt+Down dir history   Alt+F8 command history   Alt+F1/F2 choose drive\n"
        "Alt+F11/F12 keyboard-focus the left / right path bar\n"
        "Alt+Enter properties               Ctrl+R refresh     Ctrl+O view mode\n"
        "Alt+. hidden files Alt+D add favorite   Shift+F9 hotlist  Alt+F7 search\n"
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
        "Ctrl+Enter console   type a command, Enter runs it (cd, pwd, clear, history, exit)\n"
        "The console runs in the focused pane's directory and keeps its backlog: Esc\n"
        "hides the window, the transcript stays for the next directory and session\n"
        "Alt+C prefill 'cd '   Alt+F8 / Alt+PgUp/PgDn command history\n\n"
        "MENUS\n"
        "F10 opens the Commands menu   Shift+F9 hotlist   Shift+F10 pane menu\n"
        "File, Commands, Options and Help sit on the menu bar\n"
        "Options > Fonts and theme... picks the font and the palette\n");
    if (!open) app->about_open = false;
    ImGui::End();
}

// ---------------------------------------------------------------------------
// The permission and ownership half of the Properties window.
// ---------------------------------------------------------------------------

// "adam (1000)", or just "1000" when the id has no entry. A file owned by a uid the
// passwd database does not know is still owned by that uid - an LDAP or container
// system is full of them - and a blank would read as nobody owning it.
#ifdef _WIN32
// Windows files are owned by a security identifier, not a uid, and the numbers a stat
// call makes up for it are always 0. Saying so beats naming a user who does not exist.
static void props_id_names(uid_t, gid_t, char* owner, size_t owner_n,
                           char* group_name, size_t group_n) {
    snprintf(owner, owner_n, "(not used on Windows)");
    snprintf(group_name, group_n, "(not used on Windows)");
}

static bool props_parse_id(const char*, bool, unsigned long*, std::string* why) {
    *why = "ownership is not set from here on Windows";
    return false;
}

static int lchown(const char*, uid_t, gid_t) {
    errno = ENOSYS;
    return -1;
}
#else
static void props_id_names(uid_t uid, gid_t gid, char* owner, size_t owner_n,
                           char* group_name, size_t group_n) {
    struct passwd* pw = getpwuid(uid);
    if (pw && pw->pw_name && *pw->pw_name)
        snprintf(owner, owner_n, "%s (%lu)", pw->pw_name, (unsigned long)uid);
    else
        snprintf(owner, owner_n, "%lu", (unsigned long)uid);
    struct group* gr = getgrgid(gid);
    if (gr && gr->gr_name && *gr->gr_name)
        snprintf(group_name, group_n, "%s (%lu)", gr->gr_name, (unsigned long)gid);
    else
        snprintf(group_name, group_n, "%lu", (unsigned long)gid);
}

// A user or group by name or by number. The number is accepted because it is what
// chown actually takes, and because the window just showed the id in parentheses -
// copying it back should work.
static bool props_parse_id(const char* text, bool group, unsigned long* out, std::string* why) {
    while (*text == ' ' || *text == '\t') text++;
    if (!*text) { *why = "empty"; return false; }
    bool numeric = true;
    for (const char* p = text; *p; p++)
        if (!isdigit((unsigned char)*p)) { numeric = false; break; }
    if (numeric) { *out = strtoul(text, nullptr, 10); return true; }
    if (group) {
        struct group* gr = getgrnam(text);
        if (!gr) { *why = "no such group"; return false; }
        *out = (unsigned long)gr->gr_gid;
        return true;
    }
    struct passwd* pw = getpwnam(text);
    if (!pw) { *why = "no such user"; return false; }
    *out = (unsigned long)pw->pw_uid;
    return true;
}
#endif

// Four octal digits, which is what the boxes show and what a person used to chmod
// will type. A leading 0 is allowed because that is how mode is written everywhere
// else; anything that is not octal is refused rather than read as something else.
static bool parse_octal_mode(const char* text, mode_t* out) {
    while (*text == ' ' || *text == '\t') text++;
    if (!*text) return false;
    if (text[0] == '0' && (text[1] == 'o' || text[1] == 'O')) text += 2;
    size_t digits = 0;
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '7') return false;
        digits++;
    }
    if (!digits || digits > 4) return false;
    unsigned long v = strtoul(text, nullptr, 8);
    if (v > 07777UL) return false;
    *out = (mode_t)v;
    return true;
}

// Everything below an entry, for "apply to everything below". Symlinked directories
// are not walked into: a tree with a link back up it would otherwise be walked for as
// long as the app was left running.
static void props_collect_below(const std::string& dir, std::vector<std::string>* out, int cap) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr && (int)out->size() < cap) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        std::string p = dir + "/" + de->d_name;
        struct stat st;
        if (lstat(p.c_str(), &st) != 0) continue;
        out->push_back(p);
        if (S_ISDIR(st.st_mode)) props_collect_below(p, out, cap);
    }
    closedir(d);
}

static void props_collect_tree(const std::string& root, std::vector<std::string>* out, int cap) {
    out->push_back(root);
    if (cap <= 0) return;
    props_collect_below(root, out, cap);
}

static void props_apply_local(AppPanes* app) {
    const std::string path = app->props_path;
    // The digits the field holds win over the boxes' last state, and a mode that
    // does not parse stops here rather than applying whatever the boxes happened to
    // be showing when the typing started.
    mode_t want = 0;
    if (!parse_octal_mode(app->props_edit_mode, &want)) {
        snprintf(app->props_note, sizeof(app->props_note),
                 "Mode \"%s\" is not four octal digits", app->props_edit_mode);
        return;
    }
    app->props_edit_bits = want;
    bool have_owner = app->props_edit_owner[0] != '\0';
    bool have_group = app->props_edit_group[0] != '\0';

    unsigned long uid = 0, gid = 0;
    std::string why;
    if (have_owner && !props_parse_id(app->props_edit_owner, false, &uid, &why)) {
        snprintf(app->props_note, sizeof(app->props_note),
                 "Owner: %s - %s", app->props_edit_owner, why.c_str());
        return;
    }
    if (have_group && !props_parse_id(app->props_edit_group, true, &gid, &why)) {
        snprintf(app->props_note, sizeof(app->props_note),
                 "Group: %s - %s", app->props_edit_group, why.c_str());
        return;
    }

    std::vector<std::string> targets;
    if (app->props_recursive) props_collect_tree(path, &targets, 5000);
    else targets.push_back(path);

    int changed = 0, failed = 0;
    std::string first_err;
    for (const std::string& t : targets) {
        struct stat st;
        if (lstat(t.c_str(), &st) != 0) { failed++; continue; }
        bool ok = true;
        if (have_owner || have_group) {
            // lchown, not chown: on the link itself, not on whatever it points at.
            // Changing the target's owner from a window about the link would be a
            // change to a file the user was not looking at.
            if (lchown(t.c_str(), (uid_t)uid, (gid_t)gid) != 0) {
                ok = false;
                if (first_err.empty())
                    first_err = (errno == EPERM) ? "not permitted (ownership changes need root)"
                                                 : strerror(errno);
            }
        }
        if (ok && S_ISREG(st.st_mode) && chmod(t.c_str(), app->props_edit_bits) != 0) {
            ok = false;
            if (first_err.empty()) first_err = strerror(errno);
        }
        // A directory's mode is set too, but its executable bit is what makes it
        // traversable, so the boxes are applied as written.
        if (ok && S_ISDIR(st.st_mode) && chmod(t.c_str(), app->props_edit_bits) != 0) {
            ok = false;
            if (first_err.empty()) first_err = strerror(errno);
        }
        // A symlink has no mode of its own on Linux: chmod follows the link, and
        // there is no lchmod to ask. Saying so is the honest half of that.
        if (ok && S_ISLNK(st.st_mode)) { /* nothing to set; ownership only */ }
        if (ok) changed++;
    }

    if (changed && !failed)
        snprintf(app->props_note, sizeof(app->props_note),
                 "Applied to %d entr%s%s", changed, changed == 1 ? "y" : "ies",
                 app->props_recursive ? " (this one and everything below)" : "");
    else if (!changed)
        snprintf(app->props_note, sizeof(app->props_note), "Nothing changed: %s",
                 first_err.empty() ? "the entry is gone" : first_err.c_str());
    else
        snprintf(app->props_note, sizeof(app->props_note),
                 "Applied to %d, %d not changed: %s", changed, failed, first_err.c_str());

    struct stat st;
    if (lstat(path.c_str(), &st) == 0) {
        app->props_mode = st.st_mode;
        app->props_uid = (unsigned long)st.st_uid;
        app->props_gid = (unsigned long)st.st_gid;
        props_id_names((uid_t)st.st_uid, (gid_t)st.st_gid,
                       app->props_owner, sizeof(app->props_owner),
                       app->props_group, sizeof(app->props_group));
        app->props_edit_bits = st.st_mode & 07777;
        snprintf(app->props_edit_mode, sizeof(app->props_edit_mode), "%04o",
                 (unsigned)(app->props_edit_bits & 07777));
        app->props_edit_owner[0] = '\0';
        app->props_edit_group[0] = '\0';
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "%s", app->props_note);
}

static void props_apply_remote(AppPanes* app) {
    mode_t want = 0;
    if (!parse_octal_mode(app->props_edit_mode, &want)) {
        snprintf(app->props_note, sizeof(app->props_note),
                 "Mode \"%s\" is not four octal digits", app->props_edit_mode);
        return;
    }
    app->props_edit_bits = want;
    const NetSite* site = net_site_for(app->props_remote_root);
    if (!site) {
        snprintf(app->props_note, sizeof(app->props_note),
                 "No login for %s: Ctrl+F edits the saved sites", app->props_remote_root);
        return;
    }
    std::string err;
    if (!net_chmod(*site, app->props_remote_path, (int)(app->props_edit_bits & 07777), &err)) {
        snprintf(app->props_note, sizeof(app->props_note), "%s", err.c_str());
    } else {
        snprintf(app->props_note, sizeof(app->props_note), "Mode set to %04o",
                 (unsigned)(app->props_edit_bits & 07777));
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "%s", app->props_note);
}

// Properties dialog (Alt+Enter): recursive count + size of the selected entry, and
// the permissions, owner and group it carries.
static void open_properties(AppPanes* app, Pane& pane) {
    if (in_remote(pane)) {
        // Counting a remote tree would mean downloading it. What the listing
        // already brought back - this row's mode, size and time - is what shows.
        if (pane.selected < 0 || pane.selected >= pane.entry_count) return;
        FileEntry* e = pane.entries[pane.selected];
        std::string vroot, vrpath, rpath;
        if (net_split_virtual(pane.current_dir, &vroot, &vrpath)) {
            rpath = remote_row_path(pane, e->name);
            snprintf(app->props_path, sizeof(app->props_path), "%s", net_virtual_path(vroot, rpath).c_str());
        } else {
            snprintf(app->props_path, sizeof(app->props_path), "%s", pane.current_dir);
        }
        app->props_size = e->size;
        app->props_files = S_ISDIR(e->mode) ? 0 : 1;
        app->props_dirs = S_ISDIR(e->mode) ? 1 : 0;
        // A site's listing carries the mode - MLSD's unix.mode, or what the LIST
        // dialect showed - and carries no owner at all. FTP has no way to set one,
        // so the window says that instead of hiding the two fields.
        app->props_remote = true;
        app->props_mode = e->mode & 07777;
        app->props_edit_bits = app->props_mode;
        snprintf(app->props_edit_mode, sizeof(app->props_edit_mode), "%04o",
                 (unsigned)(app->props_edit_bits & 07777));
        snprintf(app->props_remote_root, sizeof(app->props_remote_root), "%s", vroot.c_str());
        snprintf(app->props_remote_path, sizeof(app->props_remote_path), "%s", rpath.c_str());
        app->props_owner[0] = '\0';
        snprintf(app->props_group, sizeof(app->props_group), "FTP has no owner or group");
        app->props_edit_owner[0] = '\0';
        app->props_edit_group[0] = '\0';
        app->props_recursive = false;
        app->props_note[0] = '\0';
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
    app->props_remote = false;
    app->props_remote_root[0] = '\0';
    app->props_remote_path[0] = '\0';
    struct stat st;
    if (lstat(full, &st) == 0) {
        app->props_mode = st.st_mode;
        app->props_uid = (unsigned long)st.st_uid;
        app->props_gid = (unsigned long)st.st_gid;
        props_id_names((uid_t)st.st_uid, (gid_t)st.st_gid,
                       app->props_owner, sizeof(app->props_owner),
                       app->props_group, sizeof(app->props_group));
        app->props_edit_bits = st.st_mode & 07777;
    } else {
        app->props_mode = 0;
        app->props_edit_bits = 0;
        app->props_owner[0] = '\0';
        app->props_group[0] = '\0';
    }
    snprintf(app->props_edit_mode, sizeof(app->props_edit_mode), "%04o",
             (unsigned)(app->props_edit_bits & 07777));
    app->props_edit_owner[0] = '\0';
    app->props_edit_group[0] = '\0';
    app->props_recursive = false;
    app->props_note[0] = '\0';
    app->props_open = true;
}

static void render_properties(AppPanes* app) {
    if (!app->props_open) return;
    app->props_field_active = false;   // re-claimed below if a field has the keyboard
    ImGui::SetNextWindowSize(ImVec2(460, 320), ImGuiCond_FirstUseEver);
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

    // ---- permissions, owner and group ----
    // The boxes and the octal field are two views of one value. Ticking a box
    // rewrites the digits; typing digits rewrites the boxes. Two fields that could
    // disagree with each other would leave the user guessing which one Apply read.
    ImGui::Separator();
    char octal[16];
    snprintf(octal, sizeof(octal), "%04o", (unsigned)(app->props_edit_bits & 07777));
    if (strcmp(app->props_edit_mode, octal) != 0) {
        mode_t parsed = 0;
        if (parse_octal_mode(app->props_edit_mode, &parsed)) {
            app->props_edit_bits = parsed;
            snprintf(app->props_edit_mode, sizeof(app->props_edit_mode), "%04o",
                     (unsigned)(parsed & 07777));
        }
    }

    static const char* kWho[3] = { "Owner", "Group", "Other" };
    static const mode_t kBits[3][3] = { { S_IRUSR, S_IWUSR, S_IXUSR },
                                        { S_IRGRP, S_IWGRP, S_IXGRP },
                                        { S_IROTH, S_IWOTH, S_IXOTH } };
    ImGui::Text("Mode: %s", app->props_edit_mode);
    for (int w = 0; w < 3; w++) {
        ImGui::Text("%s", kWho[w]);
        ImGui::SameLine();
        for (int b = 0; b < 3; b++) {
            bool on = (app->props_edit_bits & kBits[w][b]) != 0;
            char label[32];
            snprintf(label, sizeof(label), "%s##m%d%d",
                     b == 0 ? "r" : (b == 1 ? "w" : "x"), w, b);
            if (ImGui::Checkbox(label, &on))
                app->props_edit_bits = on ? (app->props_edit_bits | kBits[w][b])
                                          : (app->props_edit_bits & ~kBits[w][b]);
            if (b < 2) ImGui::SameLine();
        }
    }
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("##propsmode", app->props_edit_mode, sizeof(app->props_edit_mode),
                     ImGuiInputTextFlags_CharsDecimal);
    if (ImGui::IsItemActive()) app->props_field_active = true;
    ImGui::SameLine();
    mode_t typed_mode = 0;
    if (parse_octal_mode(app->props_edit_mode, &typed_mode))
        ImGui::TextUnformatted("octal");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "not four octal digits");

    ImGui::Spacing();
    if (app->props_remote) {
        // Not a limitation of this window. FTP has no command that sets an owner, and
        // SITE CHMOD is not even in the protocol proper - it is a site extension, so
        // a server can refuse it.
        ImGui::TextUnformatted("FTP has no owner or group to set, and SITE CHMOD is a "
                               "site extension: some servers refuse it.");
    } else {
        ImGui::Text("Owner: %s", app->props_owner[0] ? app->props_owner : "?");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::InputText("##propsowner", app->props_edit_owner, sizeof(app->props_edit_owner));
        if (ImGui::IsItemActive()) app->props_field_active = true;
        ImGui::SameLine();
        ImGui::TextUnformatted("name or number, empty = unchanged");

        ImGui::Text("Group: %s", app->props_group[0] ? app->props_group : "?");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::InputText("##propsgroup", app->props_edit_group, sizeof(app->props_edit_group));
        if (ImGui::IsItemActive()) app->props_field_active = true;
        ImGui::SameLine();
        ImGui::TextUnformatted("name or number, empty = unchanged");
    }

    if (!app->props_remote && app->props_dirs > 0) {
        ImGui::Checkbox("Apply to everything below", &app->props_recursive);
    }

    ImGui::Spacing();
    if (ImGui::Button(app->props_remote ? "Set mode" : "Apply")) {
        if (app->props_remote) props_apply_remote(app);
        else props_apply_local(app);
    }
    if (app->props_note[0]) {
        ImGui::SameLine();
        ImGui::TextUnformatted(app->props_note);
    }

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

// The three parts of the line. A category shows its marked subset only when it
// has something to split: "1/2 files  0 dirs" already says everything worth
// saying, and "0/0 dirs" is noise that makes the line harder to scan.
static void pane_status_parts(const PaneTotals& t, const char* total_sz, const char* marked_sz,
                              bool compact, char* out, size_t n) {
    const bool marks = t.m_files || t.m_dirs;
    const char* fs = compact ? "f" : " files";
    const char* ds = compact ? "d" : " dirs";
    char files_part[48], dirs_part[48], size_part[64];

    if (marks && t.files > 0)
        snprintf(files_part, sizeof(files_part), "%d/%d%s", t.m_files, t.files, fs);
    else
        snprintf(files_part, sizeof(files_part), "%d%s", t.files, fs);

    if (marks && t.dirs > 0)
        snprintf(dirs_part, sizeof(dirs_part), "%d/%d%s", t.m_dirs, t.dirs, ds);
    else
        snprintf(dirs_part, sizeof(dirs_part), "%d%s", t.dirs, ds);

    if (marks && t.bytes > 0)
        snprintf(size_part, sizeof(size_part), "%s/%s", marked_sz, total_sz);
    else
        snprintf(size_part, sizeof(size_part), "%s", total_sz);

    snprintf(out, n, "%s  %s  %s", files_part, dirs_part, size_part);
}

// The line itself, kept apart from drawing it, because the line *is* the feature:
// what a user reads is a string, and a string can be asserted on.
//
// With marks present the counts and the size both become marked/total, so the row
// answers "what am I about to copy" without asking Ctrl+L first. Marked first,
// always: it is the half that changes while the user is marking, and a number that
// moves on the left of a fixed one is read without hunting for it.
static void pane_status_text(const Pane& pane, const PaneTotals& t, float width,
                             char* out, size_t n) {
    if (t.files == 0 && t.dirs == 0) {
        // "0 files 0 dirs" reads like the folder is empty when it is the filter
        // that is empty, and those are different things to find out about.
        const bool filtered = pane.filter[0] != '\0' || pane.filter_active || pane.filter_exec_only;
        snprintf(out, n, "%s", filtered ? "(no rows match)" : "(empty)");
        return;
    }

    char total_sz[32], marked_sz[32];
    human_size_trim((off_t)t.bytes, total_sz, sizeof(total_sz));
    human_size_trim((off_t)t.m_bytes, marked_sz, sizeof(marked_sz));
    pane_status_parts(t, total_sz, marked_sz, false, out, n);

    // Inside an archive the bytes are the members' sizes as the archive lists
    // them, which is not the size the archive file has on disk. Saying so is the
    // difference between a total and a quietly wrong one.
    if (in_archive(pane) && t.bytes > 0 && strlen(out) + 8 < n)
        strncat(out, " packed", n - strlen(out) - 1);

    // This ImGui does not wrap Text/TextDisabled at the window edge (@2090), so a
    // line too wide for the pane has to be shortened here rather than clipped by
    // it. The compact forms drop the words before they drop the marked subset,
    // because the subset is the half the user is watching.
    if (width > 0.0f && ImGui::CalcTextSize(out).x > width) {
        pane_status_parts(t, total_sz, marked_sz, true, out, n);
        if (ImGui::CalcTextSize(out).x > width) {
            if (t.m_files || t.m_dirs)
                snprintf(out, n, "%d/%df %d/%dd", t.m_files, t.files, t.m_dirs, t.dirs);
            else
                snprintf(out, n, "%df %dd", t.files, t.dirs);
        }
    }
}

// The status row under a pane: what the files on screen add up to, how many files
// there are, and how many directories.
static void render_pane_status(const Pane& pane, bool focused, float width) {
    char line[160];
    if (pane.remote_req) {
        // Waiting on a site: say for how long, and that it can be given up on. A
        // count that keeps climbing is the difference between "slow" and "stuck".
        unsigned secs = (unsigned)((SDL_GetTicks() - pane.remote_req_ms) / 1000);
        snprintf(line, sizeof(line), "Waiting for the server... %us  (Esc stops, see the connection log)", secs);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.0f), "%s", line);
        return;
    }
    pane_status_text(pane, pane_totals(pane), width, line, sizeof(line));
    // The focused pane is the one being read, so it gets the normal text colour
    // and the other one goes dim, like the path bars above them.
    if (focused) ImGui::TextUnformatted(line);
    else ImGui::TextDisabled("%s", line);
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
        // Wrapped like the rest of the dialog: on one line this is wider than the window.
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Hinting is not offered: this build rasterizes text with %s, which does no "
                            "hinting at all. Build against freetype2 and the choice appears here.", raster);
        ImGui::PopTextWrapPos();
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
    int  threads;              // files a copy moves at once
    bool rar_order;            // RAR volumes in the order they are read
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
    g_net_form.threads = NetSite().threads;
    g_net_form.rar_order = NetSite().rar_order;
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
    g_net_form.threads = s.threads;
    g_net_form.rar_order = s.rar_order;
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
    out->threads = xfer_clamp_threads(g_net_form.threads);
    out->rar_order = g_net_form.rar_order;
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
        if (net_site_root(s) == root) { remote_forget(&s); s = site; return; }
    for (NetSite& s : app->remote_sites)
        if (net_site_root(s) == root) { remote_forget(&s); s = site; return; }
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
    // Watching the conversation is the point of the log, and the connect is where
    // a server is most often slow, so the window comes up with it.
    app->netlog_open = true;
    navigate_to(app, &active, target.c_str());
    // The listing is on its way from the worker. Whether the connect worked is
    // known when it arrives, and remote_connect_done() finishes the job then.
    if (active.remote_req) {
        app->remote_connect_req = active.remote_req;
        snprintf(app->remote_connect_back, sizeof(app->remote_connect_back), "%s", back);
        app->remote_connect_target = target;
        snprintf(app->status_msg, sizeof(app->status_msg), "Connecting to %s...",
                 net_site_root(site).c_str());
        return;
    }
    // No listing was even asked for; refresh_pane() left the reason behind, and
    // it reaches the status line from there.
    navigate_to(app, &active, back);
}

// The first listing of a connect came back. A failure puts the pane back where
// it was and leaves the dialogs open, because there the reason is on screen and
// the fields are what needs fixing; a success closes them.
static void remote_connect_done(AppPanes* app, Pane* pane, bool ok) {
    app->remote_connect_req = 0;
    if (!ok) {
        navigate_to(app, pane, app->remote_connect_back);
        return;
    }
    snprintf(app->status_msg, sizeof(app->status_msg), "%s", app->remote_connect_target.c_str());
    // A connect that worked closes the windows that offered it: a manager left
    // open over a pane that is already showing the site is a second window with
    // nothing left to decide.
    app->remote_mgr_open = false;
    app->remote_quick_open = false;
    app->remote_quick[0] = '\0';
    g_net_form.open = false;
    app->remote_edit = -1;
    app->remote_note[0] = '\0';
}

// Put the listings the worker finished since the last frame into their panes. A
// result is only used when its pane still wants it and is still on the
// directory it was asked for; anything else is a listing the user walked away
// from, and is dropped.
static void browse_reap(AppPanes* app) {
    std::vector<BrowseDone> done;
    browse_take(&done);
    for (BrowseDone& d : done) {
        Pane* p = d.pane == &app->left ? &app->left : d.pane == &app->right ? &app->right : nullptr;
        if (!p || p->remote_req != d.id || d.dir != p->current_dir) continue;
        p->remote_req = 0;
        bool connecting = app->remote_connect_req == d.id;
        if (!d.ok) {
            // Whatever the pane shows stays: ".." for a directory it just walked
            // into, the old rows for a reread.
            g_net_note = d.err;
            if (connecting) remote_connect_done(app, p, false);
            continue;
        }
        // A reread keeps the cursor on the file it was on; a directory just
        // entered gets the cursor it had when it was last left.
        std::string keep;
        if (p->entry_count > 1 && p->selected > 0 && p->selected < p->entry_count)
            keep = p->entries[p->selected]->name;
        free_entries(p->entries, p->entry_count);
        remote_rows_to_entries(d.rows, &p->entries, &p->entry_count);
        p->selected = 0;
        p->anchor = 0;
        p->list_gen++;
        p->marked.assign(p->entry_count, false);
        p->dir_size.assign(p->entry_count, -1);
        p->mark_undo.clear();
        rebuild_view(*p);
        bool placed = false;
        for (int i = 0; !keep.empty() && i < p->entry_count; i++) {
            if (keep == p->entries[i]->name) { p->selected = i; p->scroll_request = true; placed = true; break; }
        }
        if (!placed) restore_selection(p);
        if (connecting) remote_connect_done(app, p, true);
    }
}

// Esc on a pane that is waiting for a site: stop waiting. The pane keeps what it
// shows, and the listing on the worker ends at its next progress call.
static bool browse_cancel_pane(AppPanes* app, Pane* p) {
    if (!p->remote_req) return false;
    browse_cancel(p);
    bool connecting = app->remote_connect_req == p->remote_req;
    p->remote_req = 0;
    snprintf(app->status_msg, sizeof(app->status_msg), "Stopped waiting for %s", p->remote_root);
    if (connecting) remote_connect_done(app, p, false);
    return true;
}

// Open a typed URL in the focused pane. Anything the URL leaves out comes from
// the site already known for that host, so "cd ftp.example.com/pub" after a
// logged-in visit does not ask the server for an anonymous session.
// Leave the site the focused pane is on, and close the connection it was reached
// through. ".." at the site root already leaves a site by navigation; this is the
// deliberate version, and it does the half navigation cannot do. The reason a
// remote pane links libcurl instead of shelling out is that a fresh login per
// listing makes browsing unusable, so a pane that has walked off a site should not
// keep a logged-in session to it open behind the local listing it is now showing.
static void remote_disconnect(AppPanes* app) {
    Pane& active = app->left_focus ? app->left : app->right;
    if (!in_remote(active)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "This pane is not on a site: Ctrl+D leaves one");
        return;
    }
    std::string root = active.remote_root;
    // The connection is keyed by the site root, which is exactly what the pane
    // still knows here - and what it is about to forget. A saved entry that was
    // edited or deleted mid-session is not a reason to leave a session open.
    remote_forget_root(root.c_str());
    char back[4096];
    if (active.remote_came_from[0])
        snprintf(back, sizeof(back), "%s", active.remote_came_from);
    else {
        // A pane opened straight onto a site has no local directory of its own to
        // go back to, so it goes to the one a pane always has.
        const std::string home = plat_home_dir();
        snprintf(back, sizeof(back), "%s", !home.empty() ? home.c_str() : plat_root_of(nullptr).c_str());
    }
    navigate_to(app, &active, back);
    snprintf(app->status_msg, sizeof(app->status_msg), "Disconnected from %s", root.c_str());
}

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
        site.threads = known->threads;
        site.rar_order = known->rar_order;
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
        site.threads = saved->threads;
        site.rar_order = saved->rar_order;
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
        remote_forget(base);            // an old login must not be reused after an edit
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

// ---- importing FileZilla's site manager ----
//
// The manager's "Import from FileZilla" reads FileZilla's sitemanager.xml and
// lists every site in it with a tick, so the ones worth keeping are chosen
// before anything is written. A site already in the list starts unticked;
// ticking it replaces the saved entry with FileZilla's.
struct FzImport {
    bool open = false;
    char path[1024] = "";
    std::vector<NetImported> sites;
    std::vector<char> pick;    // one per site: import it
    std::vector<char> known;   // one per site: its URL is already a saved site
    int skipped = 0;           // sites in protocols this program does not speak
    int cursor = -1;           // the row the keyboard is on
    bool cursor_moved = false; // scroll the cursor's row into view
    bool field_active = false; // the path field has the caret
    std::string err;
};
static FzImport g_fz;

static void fz_import_load(AppPanes* app) {
    g_fz.sites.clear();
    g_fz.pick.clear();
    g_fz.known.clear();
    g_fz.cursor = -1;
    net_filezilla_load(g_fz.path, &g_fz.sites, &g_fz.skipped, &g_fz.err);
    for (const NetImported& imp : g_fz.sites) {
        bool known = net_sites_find(app->remote_sites, net_site_root(imp.site)) != nullptr;
        g_fz.known.push_back(known);
        g_fz.pick.push_back(!known);
    }
    if (!g_fz.sites.empty()) g_fz.cursor = 0;
}

static void fz_import_open(AppPanes* app) {
    g_fz.open = true;
    snprintf(g_fz.path, sizeof(g_fz.path), "%s", net_filezilla_path().c_str());
    fz_import_load(app);
}

static int fz_import_picked(void) {
    int n = 0;
    for (char p : g_fz.pick) n += p ? 1 : 0;
    return n;
}

// Add the ticked sites to the list and write the sites file once. Returns how
// many were taken; with none ticked nothing is written and the window stays.
static int fz_import_commit(AppPanes* app) {
    int n = 0;
    for (size_t i = 0; i < g_fz.sites.size(); i++) {
        if (!g_fz.pick[i]) continue;
        const NetSite& site = g_fz.sites[i].site;
        std::string root = net_site_root(site);
        bool replaced = false;
        for (NetSite& s : app->remote_sites) {
            if (net_site_root(s) != root) continue;
            remote_forget(&s);            // an old login must not be reused after an edit
            s = site;
            replaced = true;
            break;
        }
        if (!replaced) app->remote_sites.push_back(site);
        n++;
    }
    if (n == 0) return 0;
    remote_sites_write(app);
    snprintf(app->status_msg, sizeof(app->status_msg), "Imported %d site%s from FileZilla",
             n, n == 1 ? "" : "s");
    g_fz = FzImport();
    return n;
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
        remote_forget(&app->remote_sites[remove_idx]);
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
        ImGui::SameLine();
        if (ImGui::Button("Import from FileZilla")) fz_import_open(app);
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
        ImGui::SetNextItemWidth(200);
        ImGui::SliderInt("transfer threads", &g_net_form.threads, XFER_MIN_THREADS,
                         XFER_MAX_THREADS);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How many files a copy to or from this site moves at once, each "
                              "over its own connection.\nA server that limits connections per "
                              "user refuses the ones over its limit.");
        ImGui::Checkbox("transfer RAR volumes in order", &g_net_form.rar_order);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A RAR set goes volume by volume - .rar, .r00, .r01 ... .s00, or "
                              ".part1.rar, .part2.rar - after the folder's other files, "
                              "instead of in the order the listing gives.");
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

static void render_fz_import(AppPanes* app) {
    if (!g_fz.open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 360, io.DisplaySize.y * 0.5f - 240),
                            ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Import from FileZilla", &open, ImGuiWindowFlags_NoCollapse);

    ImGui::SetNextItemWidth(-90);
    ImGui::InputText("##fzpath", g_fz.path, sizeof(g_fz.path));
    g_fz.field_active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("FileZilla's sitemanager.xml - a portable FileZilla keeps it beside "
                          "FileZilla.exe");
    ImGui::SameLine();
    if (ImGui::Button("Reload")) fz_import_load(app);

    if (!g_fz.err.empty()) {
        ImGui::TextWrapped("%s", g_fz.err.c_str());
    } else if (g_fz.sites.empty()) {
        ImGui::TextUnformatted("(no FTP, FTPS or SFTP sites in this file)");
    }
    if (g_fz.skipped > 0)
        ImGui::TextDisabled("%d site%s in other protocols (S3, WebDAV, ...) left out",
                            g_fz.skipped, g_fz.skipped == 1 ? "" : "s");

    float buttons_h = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::BeginChild("##fzlist", ImVec2(0, -buttons_h), ImGuiChildFlags_Borders);
    for (int i = 0; i < (int)g_fz.sites.size(); i++) {
        const NetImported& imp = g_fz.sites[i];
        ImGui::PushID(i);
        bool pick = g_fz.pick[i] != 0;
        if (ImGui::Checkbox("##pick", &pick)) { g_fz.pick[i] = pick; g_fz.cursor = i; }
        ImGui::SameLine();
        std::string root = net_site_root(imp.site);
        char label[1024];
        snprintf(label, sizeof(label), "%s  %s%s", imp.site.name.c_str(), root.c_str(),
                 imp.site.path.c_str());
        if (ImGui::Selectable(label, g_fz.cursor == i)) {
            g_fz.cursor = i;
            g_fz.pick[i] = !g_fz.pick[i];
        }
        if (g_fz.cursor_moved && g_fz.cursor == i) ImGui::SetScrollHereY(0.5f);
        std::string note = imp.note;
        if (g_fz.known[i]) note = note.empty() ? "already saved: importing replaces it"
                                               : "already saved: importing replaces it; " + note;
        if (!note.empty()) {
            ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
            ImGui::TextDisabled("%s", note.c_str());
            ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        }
        ImGui::PopID();
    }
    g_fz.cursor_moved = false;
    ImGui::EndChild();

    if (ImGui::Button("All")) for (char& p : g_fz.pick) p = 1;
    ImGui::SameLine();
    if (ImGui::Button("None")) for (char& p : g_fz.pick) p = 0;
    ImGui::SameLine();
    int picked = fz_import_picked();
    char import_label[64];
    snprintf(import_label, sizeof(import_label), "Import %d site%s###fzimport", picked,
             picked == 1 ? "" : "s");
    ImGui::BeginDisabled(picked == 0);
    if (ImGui::Button(import_label)) fz_import_commit(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) open = false;
    ImGui::TextDisabled("Space ticks a row, Ctrl+A all or none, Enter imports. A password "
                        "comes across as a remembered one.");

    if (!open) g_fz = FzImport();
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
    if (ImGui::MenuItem("Show file icons", nullptr, app->show_icons))
        app->show_icons = !app->show_icons;
    if (ImGui::MenuItem("Show launch bar", nullptr, app->show_launchbar))
        app->show_launchbar = !app->show_launchbar;
    if (ImGui::MenuItem("Define columns", "Shift+F1")) app->columns_open = !app->columns_open;

    if (ImGui::BeginMenu("Sort by")) {
        static const int         keys[5] = { FS_SORT_NAME, FS_SORT_EXT, FS_SORT_DATE, FS_SORT_SIZE, FS_SORT_NONE };
        static const char*       names[5] = { "Name", "Extension", "Date", "Size", "Unsorted" };
        static const char*       hints[5] = { "Ctrl+F3", "Ctrl+F4", "Ctrl+F5", "Ctrl+F6", "Ctrl+F7" };
        for (int i = 0; i < 5; i++) {
            if (ImGui::MenuItem(names[i], hints[i], fs_get_sort_mode() == keys[i]))
                set_sort(app, keys[i]);
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
    // Greyed out when the focused pane is not on a site, so the entry says which
    // pane it would act on instead of doing nothing when it is clicked.
    if (ImGui::MenuItem("Disconnect from site", "Ctrl+D", in_remote(active)))
        remote_disconnect(app);
    if (ImGui::MenuItem("Connection log", nullptr, app->netlog_open))
        app->netlog_open = !app->netlog_open;
    // No key binding: every plausible one still needs the collision audit
    // FEATURES_PLAN.md describes before it can be claimed.
    if (ImGui::MenuItem("Verify SFV")) sfv_run_for_pane(app);
    if (ImGui::MenuItem("Visited directories", "Alt+Down")) app->dirhist_open = !app->dirhist_open;
    if (ImGui::MenuItem("Console", "Ctrl+Enter")) console_show(app, true);
    if (ImGui::MenuItem("Command history", "Alt+F8")) {
        console_show(app, true);
        app->console_hist_open = !app->console_hist_open;
    }
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
        hotlist_add(app, active.current_dir);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Mark all", "Ctrl+Num+")) select_all_marks(active);
    if (ImGui::MenuItem("Clear marks", "Ctrl+Num-")) clear_marks(active);
    if (ImGui::MenuItem("Invert marks", "Num*")) invert_marks(active);
    if (ImGui::MenuItem("Switch to other pane", "Ctrl+I")) app->left_focus = !app->left_focus;
    if (ImGui::MenuItem("Current path to console", "Ctrl+P")) console_put_path(app, active);
    if (ImGui::MenuItem("Swap panes", "Ctrl+U")) swap_panes(app, false);
    if (ImGui::MenuItem("Branch view", "Ctrl+B")) branch_toggle(app, active, false);
    if (ImGui::MenuItem("Branch marked dirs", "Ctrl+Shift+B")) branch_toggle(app, active, true);
    if (ImGui::MenuItem("Go to root", "Ctrl+,"))
        navigate_to(app, &active, plat_root_of(active.current_dir).c_str());
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

// ---------------------------------------------------------------------------
// Launch bar
// ---------------------------------------------------------------------------

#ifndef _WIN32
// Through a pointer for the same reason as g_open_default: a test can see what
// would have been started.
static bool (*g_spawn_detached)(const std::vector<std::string>&, std::string*) = plat_spawn_detached;
#endif

// A button was clicked. A folder opens in the focused pane, as TC's folder
// buttons do. Anything else goes to the desktop, which starts a program or a
// shortcut and opens a document in its program - except a Linux binary, which
// xdg-open would open as a document, so that one is started directly.
static void launch_item(AppPanes* app, int i) {
    if (i < 0 || i >= (int)app->launch_items.size()) return;
    const std::string path = app->launch_items[i].path;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 "%s is not there any more (right-click the button to remove it)", path.c_str());
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        Pane& active = app->left_focus ? app->left : app->right;
        navigate_to(app, &active, path.c_str());
        return;
    }
#ifndef _WIN32
    size_t slash = path.find_last_of('/');
    const char* base = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    if (is_executable(base, st.st_mode)) {
        std::string err;
        if (!g_spawn_detached({ path }, &err))
            snprintf(app->status_msg, sizeof(app->status_msg), "Cannot run %s: %s", path.c_str(), err.c_str());
        else
            snprintf(app->status_msg, sizeof(app->status_msg), "Started %s", launch_label(path).c_str());
        return;
    }
#endif
    open_with_default(app, path.c_str());
}

// Point the focused pane at the folder a button's file is in, cursor on it.
static void launch_show_in_pane(AppPanes* app, int i) {
    if (i < 0 || i >= (int)app->launch_items.size()) return;
    const std::string& path = app->launch_items[i].path;
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return;
    std::string dir = slash == 0 ? "/" : path.substr(0, slash);
    if (dir.size() == 2 && dir[1] == ':') dir += "/";
    Pane& active = app->left_focus ? app->left : app->right;
    navigate_to(app, &active, dir.c_str());
    const std::string base = path.substr(slash + 1);
    for (int j = 0; j < active.entry_count; j++)
        if (strcmp(active.entries[j]->name, base.c_str()) == 0) {
            active.selected = j;
            active.scroll_request = true;
            break;
        }
}

// The bar: one icon button per item, under the menu. A click starts the item,
// dragging a button moves it, and a file dragged onto the bar - from a pane or
// from Explorer - becomes a new button where it is dropped.
static void render_launch_bar(AppPanes* app) {
    if (!app->show_launchbar) { app->launch_bar_max_y = -1; return; }
    const float s = floorf(ImGui::GetTextLineHeight() * 2.0f);   // the icon, TC's 24-32 px
    const float pad = 3.0f;
    const float bw = s + 2 * pad;                                 // the button
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 2.0f));
    ImGui::BeginChild("##launchbar", ImVec2(0, bw + 4.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    dl = ImGui::GetWindowDrawList();   // the bar's own, not the main window's
    ImVec2 bar_min = ImGui::GetWindowPos();
    ImVec2 bar_max = ImVec2(bar_min.x + ImGui::GetWindowWidth(), bar_min.y + ImGui::GetWindowHeight());
    app->launch_bar_min_y = bar_min.y;
    app->launch_bar_max_y = bar_max.y;

    int launch = -1, remove = -1, show = -1, move_from = -1, move_to = -1, add_at = -1;
    std::string add_path;
    const int n = (int)app->launch_items.size();
    if (n == 0) {
        ImGui::SetCursorPosY((bw + 4.0f - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextDisabled("Launch bar: drag programs, folders or files here, from a pane or from Explorer");
    }
    for (int i = 0; i < n; i++) {
        const AppPanes::LaunchItem& it = app->launch_items[i];
        ImGui::PushID(i);
        if (i) ImGui::SameLine(0.0f, 4.0f);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("b", ImVec2(bw, bw))) launch = i;
        bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (hovered || held)
            dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bw),
                              ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), 3.0f);

        size_t slash = it.path.find_last_of('/');
        std::string dir  = slash == std::string::npos ? "" : it.path.substr(0, slash);
        std::string name = slash == std::string::npos ? it.path : it.path.substr(slash + 1);
        ImVec2 ip(floorf(p.x + pad), floorf(p.y + pad));
        unsigned long long tex = icon_shell_large(dir.c_str(), name.c_str(), it.is_dir);
        if (tex)
            dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)tex), ip, ImVec2(ip.x + s, ip.y + s));
        else
            draw_kind_icon(dl, icon_kind(name.c_str(), it.is_dir, it.is_exec), ip, s);
        ImGui::SetItemTooltip("%s\n%s", launch_label(it.path).c_str(), it.path.c_str());

        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(k_dnd_launch, &i, sizeof(i));
            ImGui::Text("%s", launch_label(it.path).c_str());
            ImGui::EndDragDropSource();
        }
        // Dropped on a button: the dropped thing goes in front of it.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(k_dnd_launch)) {
                move_from = *(const int*)pl->Data;
                move_to = i;
            }
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(k_dnd_path)) {
                add_path = (const char*)pl->Data;
                add_at = i;
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("ctx")) {
            ImGui::TextDisabled("%s", launch_label(it.path).c_str());
            ImGui::Separator();
            if (ImGui::MenuItem(it.is_dir ? "Open in pane" : "Start")) launch = i;
            if (!it.is_dir && ImGui::MenuItem("Show in pane")) show = i;
            if (ImGui::MenuItem("Move left", nullptr, false, i > 0)) { move_from = i; move_to = i - 1; }
            if (ImGui::MenuItem("Move right", nullptr, false, i + 1 < n)) { move_from = i; move_to = i + 2; }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove from launch bar")) remove = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Dropped on the bar but not on a button: at the end.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(k_dnd_launch)) {
            move_from = *(const int*)pl->Data;
            move_to = n;
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(k_dnd_path)) {
            add_path = (const char*)pl->Data;
            add_at = n;
        }
        ImGui::EndDragDropTarget();
    }
    // Something from Explorer over the bar: outline it, as ImGui does for its own.
    if (app->os_drag_active && app->os_drag_y >= bar_min.y && app->os_drag_y < bar_max.y)
        ImGui::GetWindowDrawList()->AddRect(bar_min, bar_max, ImGui::GetColorU32(ImGuiCol_DragDropTarget),
                                            0.0f, 0, 2.0f);
    ImGui::Separator();

    // Acted on after the loop, which reads the list it would otherwise change.
    if (!add_path.empty()) launchbar_add(app, add_path.c_str(), add_at);
    if (move_from >= 0) launchbar_move(app, move_from, move_to);
    if (remove >= 0) launchbar_remove(app, remove);
    if (show >= 0) launch_show_in_pane(app, show);
    if (launch >= 0) launch_item(app, launch);
}

void app_panes_drag_over(AppPanes* app, bool active, float x, float y) {
    app->os_drag_active = active;
    app->os_drag_x = x;
    app->os_drag_y = y;
}

void app_panes_drop_file(AppPanes* app, const char* path, float x, float y) {
    (void)x;
    if (app->show_launchbar && y >= app->launch_bar_min_y && y < app->launch_bar_max_y) {
        launchbar_add(app, path);
        return;
    }
    snprintf(app->status_msg, sizeof(app->status_msg),
             app->show_launchbar ? "Drop it on the launch bar to add a button for it"
                                 : "Turn on Options > Show launch bar to add buttons by dropping files");
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
    sfv_reap(app);

    // Same for the quick view: a preview that arrived from a site or an archive
    // is filed under the path it was asked for here, and its thread is joined
    // here, where waiting for a finished thread costs nothing.
    qv_reap(app);

    // And for the console: a command that has finished stops saying "running" now,
    // not when the next one is typed - before, Esc on a finished command still
    // "cancelled" it, and the transcript said it was interrupted.
    console_reap(app);

    // ImGui answers one keyboard-focus request per frame, and the last one
    // submitted wins. Any field that wants the caret has to claim it here and be
    // left alone after.
    app->focus_claimed = false;

    // Site listings that came back from the worker go into their panes here.
    browse_reap(app);

    // A fresh allowance of shell icons that cost a disk read (see icons.h).
    icon_frame();

    // A listing that failed did so inside refresh_pane() or on the worker, neither
    // of which has a status line of its own. The reason waits there and lands
    // here, where the user sees it instead of an empty pane.
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
        // A close request consumed here must not leave the popup flagged alive:
        // the next F10 would read that flag as "a menu is up" and dismiss a menu
        // that is no longer on screen instead of opening a new one.
        if (app->popup_close_request) ImGui::CloseCurrentPopup();
        else app->menu_alive = true;
        render_commands_items(app);
        ImGui::EndPopup();
    }

    // Under the menu; the panes below measure their height from where it ends.
    render_launch_bar(app);

    // Dual pane layout: 2-column table guarantees a 50/50 split.
    // Vertical budget comes from the window height and cursor position
    // (GetContentRegionAvail over-reports here); the command bar is then
    // pinned to the window bottom so it can never spill outside.
    const float frame    = ImGui::GetFrameHeightWithSpacing();
    const float bar_h    = frame + 8.0f;  // separator + status line
    // The transfer strip, when there are transfers. Its rows come out of the
    // panes' height, which is why the room is measured before the panes are laid
    // out and not after they have taken all of it.
    const float xfer_h   = bg_strip_height(app, frame);
    const float tabbar_h = frame;         // tab strip
    const float title_h  = frame;         // pane path bar
    const float sep_h    = 6.0f;          // separator under the path bar
    const float filt_h   = frame + 2.0f;  // quick filter row, when shown
    const float info_h   = frame;         // status row under each pane's list
    const float win_h    = ImGui::GetWindowHeight();
    const float table_h  = win_h - ImGui::GetCursorPosY() - bar_h - xfer_h;
    const float base_h   = table_h - tabbar_h - title_h - sep_h;
    const bool  l_filter = left.filter_active || left.filter[0] != '\0' || left.filter_exec_only;
    const bool  r_filter = right.filter_active || right.filter[0] != '\0' || right.filter_exec_only;
    // Clamped, because this budget already spends five rows and the status row
    // is a sixth: a short window should lose list rows and keep the line, not
    // hand BeginChild a negative height.
    const float left_child_h  = ImMax(2.0f * frame, base_h - (l_filter ? filt_h : 0.0f) - info_h);
    const float right_child_h = ImMax(2.0f * frame, base_h - (r_filter ? filt_h : 0.0f) - info_h);
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

    // Drawn in the table cell rather than inside the list: the quick view panel
    // replaces the list, and the tree modes render their own rows, so this is
    // the one place all three paths have already been taken. It also keeps the
    // pane and context menus anchored to the list instead of to this row.
    render_pane_status(left, left_focus, left_w);

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

    render_pane_status(right, !left_focus, right_w);

    ImGui::EndTable();

    // The transfer list, between the panes and the command line. It draws
    // nothing at all while no transfer is running or remembered.
    render_transfers(app, xfer_h);
    render_sfv_reports(app);

    // Bottom bar: pinned to the window bottom so it can never overflow. The op
    // prompt used to live here as a one-line field, then the command line, and now
    // the console is a window of its own, so this row is the status line only - the
    // information TC's status line carries.
    ImGui::SetCursorPosY(win_h - bar_h);
    ImGui::Separator();
    {
        // With no status message the line reports marks and the clipboard, like the
        // TC status line, and names the one key that used to be this row.
        char info[160];
        info[0] = '\0';
        if (app->status_msg[0] == '\0') {
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
                snprintf(info, sizeof(info), "Marked: %d   Clipboard: %d (%s)   Ctrl+Enter console",
                         marks, (int)app->clipboard.paths.size(),
                         app->clipboard.cut ? "cut" : "copy");
            else if (marks > 0)
                snprintf(info, sizeof(info), "Marked: %d   Ctrl+Enter console", marks);
            else
                snprintf(info, sizeof(info), "Ctrl+Enter console");
        }
        const char* shown = app->status_msg[0] ? app->status_msg : info;
        if (shown[0]) {
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
    render_console(app);
    render_netlog(app);
    render_hotlist(app);
    render_dirhist(app);
    render_volumes(app);
    render_columns_dialog(app);
    render_pack_dialog(app);
    render_op_dialog(app);
    render_tree(app);
    render_search(app);
    render_choice(app);
    render_confirm(app);
    render_about(app);
    render_properties(app);
    render_options(app);
    render_remote_manager(app);
    render_fz_import(app);
    render_remote_quick(app);

    // The close request is a one-frame signal and every popup above has had its
    // chance to consume it. Left standing, it closes the next popup on the very
    // frame that popup opens, which is what made F10 stop showing the Commands
    // menu after the first time it dismissed one.
    app->popup_close_request = false;
}

AppPanes* app_panes_init() {
    AppPanes* app = new AppPanes();
    std::string home_dir = plat_home_dir();
    if (home_dir.empty()) home_dir = plat_root_of(nullptr);
    const char* home = home_dir.c_str();

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
    launchbar_load(app);
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
    cfg->show_icons = app->show_icons ? 1 : 0;
    cfg->show_launchbar = app->show_launchbar ? 1 : 0;
    cfg->trash = app->trash_mode ? 1 : 0;
    cfg->sort_mode = fs_get_sort_mode();
    cfg->sort_desc = fs_get_sort_desc() ? 1 : 0;
    snprintf(cfg->filter_preset, sizeof(cfg->filter_preset), "%s", app->filter_preset);
    snprintf(cfg->columns, sizeof(cfg->columns), "%s", app->columns);
    // The F3 window's size, so the preview is as big as the user left it. Its
    // position is not saved: coordinates from a monitor that has gone leave a
    // window that cannot be grabbed.
    cfg->qv_w = (int)app->qv_w;
    cfg->qv_h = (int)app->qv_h;
    cfg->qv_images = app->qv_images_on ? 1 : 0;
    cfg->console_w = (int)app->console_w;
    cfg->console_h = (int)app->console_h;
    // The console backlog, oldest first, and only its tail: a session that ran a
    // command printing a megabyte is not worth a megabyte of config file, but the
    // first screenful of it is worth having tomorrow.
    cfg->console_entries.clear();
    size_t skip = 0;
    if (app->console_log.size() > (size_t)MC_CONSOLE_SAVE_ENTRIES)
        skip = app->console_log.size() - (size_t)MC_CONSOLE_SAVE_ENTRIES;
    for (size_t i = skip; i < app->console_log.size(); i++) {
        // A command that was still running when the app closed has no answer to
        // save, and saving a half of one would read like a command that printed
        // less than it did.
        if (app->console_job && i + 1 == app->console_log.size()) continue;
        const ConsoleEntry& e = app->console_log[i];
        MCConsoleEntry m;
        m.cwd = e.cwd; m.cmd = e.cmd; m.out = e.out; m.code = e.code; m.builtin = e.builtin;
        if (m.out.size() > MC_CONSOLE_SAVE_OUTPUT) {
            m.out.resize(MC_CONSOLE_SAVE_OUTPUT);
            trim_partial_utf8(m.out);
            m.out += "\n...";
        }
        cfg->console_entries.push_back(m);
    }
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
    app->show_icons = cfg->show_icons != 0;
    app->show_launchbar = cfg->show_launchbar != 0;
    app->trash_mode = cfg->trash != 0;
    fs_set_show_hidden(app->show_hidden);
    if (cfg->sort_mode >= FS_SORT_NAME && cfg->sort_mode <= FS_SORT_NONE) fs_set_sort_mode(cfg->sort_mode);
    fs_set_sort_desc(cfg->sort_desc != 0);
    snprintf(app->filter_preset, sizeof(app->filter_preset), "%s", cfg->filter_preset);
    // "ask" | "always" | "never", already validated by mc_config_load: a value it did
    // not recognise came back as "ask".
    snprintf(app->sfv_verify, sizeof(app->sfv_verify), "%s",
             cfg->sfv_verify[0] ? cfg->sfv_verify : "ask");
    snprintf(app->preview_codepage, sizeof(app->preview_codepage), "%s",
             cfg->preview_codepage[0] ? cfg->preview_codepage : "auto");
    snprintf(app->term_pref, sizeof(app->term_pref), "%s", cfg->terminal);
    if (cfg->columns[0]) snprintf(app->columns, sizeof(app->columns), "%s", cfg->columns);
    // A config that never carried these, or carries a 0 from a hand-edited file,
    // leaves the size the app was built with. mc_config_load has already clamped a
    // window too small to be usable back to that default.
    if (cfg->qv_w > 0 && cfg->qv_h > 0) {
        app->qv_w = (float)cfg->qv_w;
        app->qv_h = (float)cfg->qv_h;
    }
    // Pictures in quick view are on unless the config says otherwise. Read as a
    // switch, not as a preference to guess at: a config that never carried the line
    // leaves the behaviour the app was built with, which is the new one.
    app->qv_images_on = cfg->qv_images ? 1 : 0;
    if (cfg->console_w > 0 && cfg->console_h > 0) {
        app->console_w = (float)cfg->console_w;
        app->console_h = (float)cfg->console_h;
    }
    // The backlog from the last session, oldest first. It is restored whether or
    // not the window is opened: the transcript is the thing that outlives a
    // directory, and a session that ends on a site or in a subdirectory still has
    // to find yesterday's commands where it left them. The commands seed the
    // history too, so Up still recalls what was run before this process started.
    app->console_log.clear();
    app->console_hist.clear();
    for (const MCConsoleEntry& m : cfg->console_entries) {
        if (m.cmd.empty()) continue;
        ConsoleEntry e;
        e.cwd = m.cwd; e.cmd = m.cmd; e.out = m.out; e.code = m.code; e.builtin = m.builtin;
        app->console_log.push_back(e);
        if (app->console_hist.empty() || app->console_hist.back() != m.cmd)
            app->console_hist.push_back(m.cmd);
    }
    if ((int)app->console_log.size() > MC_CONSOLE_MAX_ENTRIES)
        app->console_log.erase(app->console_log.begin(),
                               app->console_log.end() - MC_CONSOLE_MAX_ENTRIES);
    if ((int)app->console_hist.size() > MC_CONSOLE_MAX_HIST)
        app->console_hist.erase(app->console_hist.begin(),
                                app->console_hist.end() - MC_CONSOLE_MAX_HIST);
    app->console_hist_pos = (int)app->console_hist.size();
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
            // A saved tab can carry a doubled slash an older build wrote. Fixed here,
            // on the way in, rather than only when the pane walks onto it: otherwise
            // the tab's label and the next config write keep the old shape, and an
            // inactive tab stays a different string from the same directory.
            char clean[4096];
            normalize_pane_path(tabs[t], clean, sizeof(clean));
            if (!tab_dir_still_there(clean)) continue;
            if (t == active) new_active = n;
            snprintf(dirs[n], 4096, "%s", clean);
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
    // The row icons are GL textures, like the previews below.
    icon_free_all();
    // A .sfv still hashing when the app closes: a std::thread that is still joinable
    // when AppPanes is destroyed ends the process, so the hash is stopped and waited
    // for rather than abandoned.
    for (auto& held : app->sfv_jobs) {
        held->cancel.store(true);
        if (held->worker.joinable()) held->worker.join();
    }
    app->sfv_jobs.clear();
    // Decoded previews hold GL textures. Freeing them while the context is still up
    // is the polite half; if the context is already gone the textures went with it
    // and tex_free() knows not to call into a dead one.
    for (std::pair<std::string, std::unique_ptr<QvImage>>& held : app->qv_images)
        tex_free(held.second->tex);
    app->qv_images.clear();
    // A console command that was still running when the app closed: ask the shell
    // to stop and wait for the worker, because a std::thread that is still joinable
    // when AppPanes is destroyed ends the process.
    if (app->console_job) {
        app->console_job->cancel.store(true);
        if (app->console_job->worker.joinable()) app->console_job->worker.join();
        app->console_job.reset();
    }
    // The listing worker is inside libcurl too, possibly waiting on a dead host;
    // stopping it makes its progress callback say no, so this is quick.
    browse_stop();
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
            if (plan.archive.empty())
                snprintf(app->confirm_text, sizeof(app->confirm_text),
                         "%d target%s in %s already exist%s. Overwrite them, leave them "
                         "alone, or write beside them?", existing, existing == 1 ? "" : "s",
                         plan.dst_dir.c_str(), existing == 1 ? "s" : "");
            else
                snprintf(app->confirm_text, sizeof(app->confirm_text),
                         "%d target%s in %s already exist%s. Overwrite them, or leave "
                         "them alone?", existing, existing == 1 ? "" : "s",
                         plan.dst_dir.c_str(), existing == 1 ? "s" : "");
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
    // The drive picker has no text field, and its letters pick a drive: typed into
    // the command line behind it they would be text nobody meant.
    if (app && app->volumes_open) return true;
    // True while a pane's quick filter field holds the keyboard: typing then
    // belongs to the filter, not to the marks or the command line.
    auto filter_focused = [app]() {
        return app->left.filter_field_active || app->right.filter_field_active;
    };
    // While the op dialog is up its field owns the text keys: arrows, Home, End
    // and Delete move the caret inside a long typed path instead of walking the
    // file list.
    // Once Tab has put the keyboard on one of the buttons, the arrows move between
    // the buttons instead.
    if (app && app->op != OP_NONE) {
        switch (key) {
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
            return app->op_focus != OF_FIELD;
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
    // The FileZilla import's path field holds a path, and a Windows one has spaces
    // in it. Away from the field the arrows and Space are the list's, even with
    // the manager's site form open underneath.
    if (app && g_fz.open && g_fz.field_active) {
        switch (key) {
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
        case SDLK_HOME: case SDLK_END: case SDLK_DELETE: case SDLK_BACKSPACE:
        case SDLK_SPACE:
            return false;
        default: break;
        }
    }
    // And for the remote dialogs. The site form and the quick-connect line are
    // text fields, so arrows, Home, End and Delete belong to the caret inside
    // them; without this the caret never moves and the pane behind the dialog
    // walks instead, which reads as the dialog ignoring what was typed.
    if (app && (app->remote_quick_open ||
                (app->remote_mgr_open && g_net_form.open && !g_fz.open))) {
        switch (key) {
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
        case SDLK_HOME: case SDLK_END: case SDLK_DELETE: case SDLK_BACKSPACE:
            return false;
        default: break;
        }
    }

    // And for the Properties window's owner, group and mode fields: a group called
    // "admin" is typed, and Delete inside it removes a letter, not a file.
    if (app && app->props_open && app->props_field_active) {
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
        return !filter_focused();                 // go to parent dir
    case SDLK_DELETE:
        // Del edits the filter text, it does not delete files
        return !filter_focused();
    case SDLK_SPACE:
        return !filter_focused();                 // TC: Space marks
    case SDLK_SLASH: case SDLK_KP_DIVIDE:
        return !filter_focused();                 // TC Num/: restore the previous marks
    case SDLK_PLUS: case SDLK_KP_PLUS:
    case SDLK_MINUS: case SDLK_KP_MINUS:
    case SDLK_ASTERISK: case SDLK_KP_MULTIPLY:
        return !filter_focused();                 // the select shortcuts
    case SDLK_C: case SDLK_X: case SDLK_V:
        // TC file clipboard keys. They belong to the panes unless the console's
        // field is holding the keyboard, and that case never gets here: main.cpp
        // hands the whole keyboard to ImGui while the console owns it, which is what
        // makes Ctrl+C and Ctrl+V copy and paste *inside* the console's own line.
        return (mod & SDL_KMOD_CTRL) && !(mod & SDL_KMOD_ALT);
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

    // ---- three-answer dialog ----
    // Enter is the recommended answer, Esc is the one that changes nothing, and
    // every other key is swallowed: while this is up a stray letter is an answer,
    // not a command to the panes behind it.
    if (app->choice != CHOICE_NONE) {
        if (key == SDLK_R || key == SDLK_RETURN || key == SDLK_KP_ENTER) app->choice_pick = 0;
        else if (key == SDLK_S) app->choice_pick = 1;
        else if (key == SDLK_ESCAPE || key == SDLK_N) app->choice_pick = 2;
        return;
    }

    // ---- modal confirm dialog ----
    // Y is the first answer, N the second, R the third, and Esc is not an answer
    // to an overwrite question at all: it cancels the operation, which is the
    // promise people already have when a confirm turns out to be the wrong question.
    if (app->confirm != CONF_NONE) {
        const bool overwrite_confirm = confirm_is_overwrite(app->confirm);
        const bool sfv_confirm = app->confirm == CONF_SFV_VERIFY;
        if (sfv_confirm) {
            // V starts the check, N leaves the files exactly as they are, A changes
            // the behaviour for the rest of this session. Esc is N: the question was
            // never asked before, and a download that is already on disk is not
            // something an accidental key should undo.
            if (key == SDLK_V || key == SDLK_RETURN || key == SDLK_KP_ENTER)
                app->pending_confirm_answer = CONF_ANS_VERIFY;
            else if (key == SDLK_N || key == SDLK_ESCAPE)
                app->pending_confirm_answer = CONF_ANS_NOT_NOW;
            else if (key == SDLK_A)
                app->pending_confirm_answer = CONF_ANS_ALWAYS;
            else return;
            return;
        }
        if (key == SDLK_Y || key == SDLK_RETURN || key == SDLK_KP_ENTER)
            app->pending_confirm_answer = overwrite_confirm ? CONF_ANS_OVERWRITE : CONF_ANS_YES;
        else if (key == SDLK_N)
            app->pending_confirm_answer = overwrite_confirm ? CONF_ANS_SKIP : CONF_ANS_NO;
        else if (key == SDLK_R && overwrite_confirm)
            app->pending_confirm_answer = CONF_ANS_RENAME;
        else if (key == SDLK_ESCAPE)
            app->pending_confirm_answer = overwrite_confirm ? CONF_ANS_CANCEL : CONF_ANS_NO;
        else return;
        return;
    }

    // ---- the operation dialog (F5, F6, F7, Shift+F4, F2) ----
    // Tab walks the dialog's controls, Enter runs whatever it is sitting on, and
    // Esc drops the dialog; every other key is either text for the field or
    // nothing. Text keys never reach here - app_key_is_special lets the editing
    // keys through so the caret, selection and clipboard in the field keep
    // working - so a stray 'd' cannot delete a file while a destination is being
    // typed. Space is deliberately not an activation key: the field keeps the caret
    // while the ring is on a button, and a space is a character in a path.
    if (app->op != OP_NONE) {
        if (key == SDLK_TAB) { op_tab(app, shift); return; }
        // On a button the arrows step the same ring; in the field they never get
        // here, because the caret has them (see app_key_is_special).
        if (app->op_focus != OF_FIELD) {
            if (key == SDLK_LEFT || key == SDLK_UP)    { op_tab(app, true);  return; }
            if (key == SDLK_RIGHT || key == SDLK_DOWN) { op_tab(app, false); return; }
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            // Shift+Enter is the keyboard half of the dialog's "Run in
            // background" button. It is raised for any copy or move and honoured
            // only if the plan turns out to cross a network: the destination is
            // not resolved until commit_op() runs, so the key cannot know yet,
            // and run_op_plan() clears the request either way.
            switch (app->op_focus) {
            case OF_CANCEL:
                app_panes_cancel_op(app);
                return;
            case OF_BG:
                if (app->op == OP_COPY || app->op == OP_MOVE) app->op_run_bg = true;
                break;
            default:
                if ((mod & SDL_KMOD_SHIFT) && (app->op == OP_COPY || app->op == OP_MOVE))
                    app->op_run_bg = true;
                break;
            }
            app_panes_commit_op(app);
        } else if (key == SDLK_ESCAPE) app_panes_cancel_op(app);
        return;
    }

    // ---- the remote dialogs (Ctrl+N quick connect, Ctrl+F site manager) ----
    // Enter and Esc are the only keys these take; every other key is text for a
    // field. In the manager, Enter commits the editor when one is showing, which
    // is what a keyboard user expects from a form, and Esc steps out of the form
    // before it closes the window.
    //
    // While a connect started from one of them is still waiting for the server,
    // Esc gives up on the connect first and leaves the dialog where it was.
    if (key == SDLK_ESCAPE && app->remote_connect_req) {
        Pane* waiting = app->left.remote_req == app->remote_connect_req ? &app->left
                      : app->right.remote_req == app->remote_connect_req ? &app->right : nullptr;
        if (waiting && browse_cancel_pane(app, waiting)) return;
        app->remote_connect_req = 0;
    }
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
    // The FileZilla import sits over the manager and takes the keys before it:
    // the arrows walk its rows, Space ticks one, Enter imports what is ticked.
    // While the path field has the caret the keys are its text, and Enter reads
    // the file that was typed.
    if (g_fz.open) {
        if (key == SDLK_ESCAPE) { g_fz = FzImport(); return; }
        if (g_fz.field_active) {
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER) fz_import_load(app);
            return;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) { fz_import_commit(app); return; }
        if (key == SDLK_SPACE || key == SDLK_INSERT) {
            if (g_fz.cursor >= 0 && g_fz.cursor < (int)g_fz.pick.size())
                g_fz.pick[g_fz.cursor] = !g_fz.pick[g_fz.cursor];
            return;
        }
        if (key == SDLK_A && (mod & SDL_KMOD_CTRL)) {
            bool all = fz_import_picked() == (int)g_fz.pick.size();
            for (char& p : g_fz.pick) p = !all;
            return;
        }
        int next = net_cursor_step(g_fz.cursor, (int)g_fz.sites.size(), key);
        if (next != g_fz.cursor) { g_fz.cursor = next; g_fz.cursor_moved = true; }
        return;
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

    // ---- Alt+F1/F2 drive picker: the arrows walk it, Enter opens the drive, a
    //      letter jumps to the drive it names, Esc closes. It is a window over the
    //      panes, so no other key reaches them; Alt+F1/F2 still go through, to close
    //      it or to turn it to the other pane. ----
    if (app->volumes_open && !((key == SDLK_F1 || key == SDLK_F2) && (mod & SDL_KMOD_ALT))) {
        StringList mnt, label;
        int drives = 0;
        volumes_rows(app, &mnt, &label, &drives);
        StringList drive_paths(mnt.begin(), mnt.begin() + drives);
        if (app->volumes_cursor < 0 || app->volumes_cursor >= (int)mnt.size())
            app->volumes_cursor = volumes_home_row(app, drive_paths);
        if (key == SDLK_ESCAPE) { app->volumes_open = false; app->volumes_cursor = -1; return; }
        // Ins adds the folder the picker's pane is in; Del takes out the favorite
        // under the cursor. A drive has nothing to remove.
        if (key == SDLK_INSERT) {
            const Pane& pane = app->volumes_target == 0 ? app->left : app->right;
            if (hotlist_add(app, pane.current_dir)) {
                app->volumes_cursor = (int)mnt.size();   // the new last row
                app->volumes_moved = true;
                snprintf(app->status_msg, sizeof(app->status_msg), "Added to favorites: %s",
                         pane.current_dir);
            } else {
                snprintf(app->status_msg, sizeof(app->status_msg), "Already a favorite: %s",
                         pane.current_dir);
            }
            return;
        }
        if (key == SDLK_DELETE) {
            volumes_remove_favorite(app, app->volumes_cursor, drives);
            return;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            volumes_pick(app, mnt, app->volumes_cursor);
            return;
        }
        if (key >= SDLK_A && key <= SDLK_Z && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            // A letter names a drive, never a favorite that happens to start with it.
            int cur = app->volumes_cursor < drives ? app->volumes_cursor : -1;
            int row = volumes_letter_row(drive_paths, cur, (char)('a' + (key - SDLK_A)));
            if (row >= 0) {
                app->volumes_cursor = row;
                app->volumes_moved = true;
#ifdef _WIN32
                // A drive letter is one drive: the letter is the pick, as in TC.
                volumes_pick(app, mnt, row);
#endif
            }
            return;
        }
        int next = net_cursor_step(app->volumes_cursor, (int)mnt.size(), key);
        if (next != app->volumes_cursor) { app->volumes_cursor = next; app->volumes_moved = true; }
        return;
    }

    // ---- other windows: Esc closes ----
    if (app->search_open && key == SDLK_ESCAPE) {
        app->search_open = false; app->search_input_active = false; return;
    }
    if (app->hotlist_open && key == SDLK_ESCAPE) { app->hotlist_open = false; return; }
    if (app->dirhist_open && key == SDLK_ESCAPE) { app->dirhist_open = false; return; }
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
    if (key == SDLK_F8 && (mod & SDL_KMOD_ALT)) {
        // The history lives inside the console window, so Alt+F8 brings the window
        // with it.
        console_show(app, true);
        app->console_hist_open = !app->console_hist_open;
        return;
    }

    // ---- TC Alt+F1/F2: choose the left/right drive ----
    // Pressed again it closes; pressed for the other pane it turns the picker to that
    // pane, with the cursor on that pane's drive.
    if ((key == SDLK_F1 || key == SDLK_F2) && (mod & SDL_KMOD_ALT)) {
        int target = key == SDLK_F1 ? 0 : 1;
        app->volumes_open = !(app->volumes_open && app->volumes_target == target);
        app->volumes_target = target;
        app->volumes_cursor = -1;
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
        char parent[4096];
        pane_parent(active.current_dir, parent, sizeof(parent));
        navigate_to(app, &active, parent);
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
        Pane& target = shift ? other : active;
        navigate_to(app, &target, plat_root_of(target.current_dir).c_str());
        return;
    }

    // ---- Alt+Down: the directories this pane has visited ----
    if (key == SDLK_DOWN && (mod & SDL_KMOD_ALT)) { app->dirhist_open = !app->dirhist_open; return; }

    // ---- directory history ----
    if (key == SDLK_LEFT  && (mod & SDL_KMOD_ALT)) { history_back(app, &active); return; }
    if (key == SDLK_RIGHT && (mod & SDL_KMOD_ALT)) { history_forward(app, &active); return; }

    // ---- command history (Alt+PgUp / Alt+PgDn) ----
    // TC walks its command line with these; the console is the command line now, so
    // they walk it and bring the window with them.
    if (key == SDLK_PAGEUP && (mod & SDL_KMOD_ALT)) {
        console_show(app, true);
        console_history_step(app, true);
        return;
    }
    if (key == SDLK_PAGEDOWN && (mod & SDL_KMOD_ALT)) {
        console_show(app, true);
        console_history_step(app, false);
        return;
    }

    // ---- hotlist ----
    if (key == SDLK_F9 && shift) { app->hotlist_open = !app->hotlist_open; return; }
    if (key == SDLK_D && (mod & SDL_KMOD_ALT)) {
        snprintf(app->status_msg, sizeof(app->status_msg),
                 hotlist_add(app, active.current_dir) ? "Added to favorites: %s"
                                                      : "Already a favorite: %s",
                 active.current_dir);
        return;
    }

    // ---- search / quick cd / hidden / view / sort ----
    // TC Alt+F7 = search files (Shift+F7 kept as an alias)
    if (key == SDLK_F7 && (shift || (mod & SDL_KMOD_ALT))) { app->search_open = !app->search_open; return; }
    if (key == SDLK_C && (mod & SDL_KMOD_ALT)) {
        // TC's quick cd: the console opens with `cd ` already typed, so the one key
        // does both halves of the gesture.
        console_show(app, true);
        if (app->console_input[0] == '\0') console_set_input(app, "cd ");
        return;
    }
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
    //      Ctrl+F7 unsorted (readdir order); the same key again reverses ----
    if ((mod & SDL_KMOD_CTRL) && key >= SDLK_F3 && key <= SDLK_F7) {
        static const int map[5] = { FS_SORT_NAME, FS_SORT_EXT, FS_SORT_DATE,
                                    FS_SORT_SIZE, FS_SORT_NONE };
        set_sort(app, map[key - SDLK_F3]);
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
    // Ctrl+D leaves the site the focused pane is on and closes its connection.
    // It is deliberately not Esc: Esc already has a queue of things it owns -
    // stopping a transfer, hiding the console, closing a panel - and a pane that
    // wants to leave a site should not have to get to the end of that queue.
    if (key == SDLK_D && (mod & SDL_KMOD_CTRL)) { remote_disconnect(app); return; }
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
    } else {
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
        if (key == SDLK_P) { console_put_path(app, active); return; }
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

    // ---- console: Up / Down recall the commands, not the file list ----
    // Only while the field holds the keyboard. The moment a click hands it back to
    // the panes, Up and Down are the cursor again.
    if (app->console_open && app->console_field_active &&
        (key == SDLK_UP || key == SDLK_DOWN) &&
        !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
        console_history_step(app, key == SDLK_UP);
        return;
    }

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
            const std::string home = plat_home_dir();
            if (!home.empty()) navigate_to(app, &active, home.c_str());
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
        char parent[4096];
        pane_parent(active.current_dir, parent, sizeof(parent));
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
    // ---- Ctrl+Enter: the console ----
    // The binding was free all along: every other Enter rule above checks its own
    // modifier, and the plain-Enter branch below ignores modifiers, so Ctrl+Enter
    // has been arriving here and behaving as a plain Enter.
    if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && (mod & SDL_KMOD_CTRL) &&
        !(mod & (SDL_KMOD_ALT | SDL_KMOD_MODE))) {
        if (!app->console_open) console_show(app, true);
        else if (app->console_field_active) console_run(app);   // Ctrl+Enter runs the line too
        else app->console_focus_request = true;                 // hand the keyboard back
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
        // Enter in the console runs the line, not the entry under the cursor.
        if (app->console_open && app->console_field_active) { console_run(app); return; }
        if (active.selected >= 0 && active.selected < active.entry_count)
            activate_entry(app, active, active.selected);
        return;
    }
    if (key == SDLK_ESCAPE) {
        // A keyboard-opened menu closes first (TC F10 toggles, Esc dismisses).
        // The local menu counts too: it is opened from the keyboard by Shift+F10
        // just like the other two, and ImGui's own Esc handling is off because
        // keyboard navigation is disabled, so nothing else would close it.
        if (app->menu_alive || app->pane_menu_alive || app->ctx_menu_alive) {
            app->popup_close_request = true;
            app->menu_open = app->pane_menu_open = false;
            app->entry_menu_open = false;
            return;
        }
        // A pane waiting on a site stops waiting.
        if (browse_cancel_pane(app, &active)) return;
        // A quick filter is cleared before the command line (TC Esc).
        if (active.filter_active || active.filter[0] || active.filter_exec_only) {
            active.filter[0] = '\0';
            active.filter_exec_only = false;
            active.filter_active = false;
            active.filter_field_active = false;
            ImGui::ClearActiveID();
            return;
        }
        // The console hides and keeps its backlog: that is the whole point of the
        // window, and it is why hiding is not destroying. A running command is
        // cancelled first, because the Esc that stops a `find` should not also close
        // the window that shows what it found so far.
        if (app->console_open) {
            if (app->console_hist_open) { app->console_hist_open = false; return; }
            if (app->console_job) {
                app->console_job->cancel.store(true);
                snprintf(app->status_msg, sizeof(app->status_msg), "Command cancelled");
                return;
            }
            app->console_open = false;
            app->console_field_active = false;
            app->console_hist_open = false;
            ImGui::ClearActiveID();   // the field loses the keyboard with the window
            return;
        }
        app->status_msg[0] = '\0';
        return;
    }
}
