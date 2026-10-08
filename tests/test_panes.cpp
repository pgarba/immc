// Logic tests for the pane helpers that do not need a display.
//
// This TU includes src/panes.cpp so the file-static helpers (quick filter
// matcher, the row->entry `view` map, marking, clipboard path helpers) can be
// exercised directly. Run through `ctest` or ./build/test-panes.

#include "../src/panes.cpp"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

static void touch_file(const std::string& dir, const char* name) {
    std::string p = dir + "/" + name;
    FILE* f = fopen(p.c_str(), "wb");
    if (f) { fputs("hello\n", f); fclose(f); }
}

static int idx_of(const Pane& p, const char* name) {
    for (int i = 0; i < p.entry_count; i++)
        if (strcmp(p.entries[i]->name, name) == 0) return i;
    return -1;
}

// Walk the list with nav_step the way the arrow keys do.
static std::vector<int> walk(const Pane& p, int from, int dir) {
    std::vector<int> out;
    int cur = from;
    while (cur >= 0) {
        out.push_back(cur);
        cur = nav_step(p, cur, dir);
    }
    return out;
}

static void test_matcher() {
    CHECK(filter_matches("alpha.txt", ""));
    CHECK(filter_matches("alpha.txt", "ph"));
    CHECK(filter_matches("alpha.txt", "PH"));          // case-insensitive
    CHECK(filter_matches("alpha.txt", "a.txt"));        // not a substring -> no
    CHECK(!filter_matches("gamma.log", "ph"));
    CHECK(filter_matches("alpha.txt", "*.txt"));        // glob
    CHECK(!filter_matches("gamma.log", "*.txt"));
    CHECK(filter_matches("a.txt", "a?txt"));
    CHECK(filter_matches("alpha.txt", "*"));
    CHECK(!glob_match("*.txt", "alpha.tx"));
}

static void test_view_and_nav(Pane& p) {
    // No filter: view is the identity map over entries.
    CHECK((int)p.view.size() == p.entry_count);
    CHECK(p.view.front() == 0);

    snprintf(p.filter, sizeof(p.filter), ".txt");
    rebuild_view(p);
    CHECK(strcmp(p.filter_applied, ".txt") == 0);

    // Every visible row matches, except ".." which always stays visible.
    for (int i : p.view) {
        const char* n = p.entries[i]->name;
        CHECK(strcmp(n, "..") == 0 || filter_matches(n, ".txt"));
    }
    CHECK(idx_of(p, "alpha.txt") >= 0);
    CHECK(std::find(p.view.begin(), p.view.end(), idx_of(p, "beta.txt")) != p.view.end());
    CHECK(std::find(p.view.begin(), p.view.end(), idx_of(p, "gamma.log")) == p.view.end());
    CHECK(std::find(p.view.begin(), p.view.end(), idx_of(p, "..")) != p.view.end());

    // Arrow-key walking visits exactly the visible rows, in view order.
    std::vector<int> down = walk(p, first_visible(p), 1);
    CHECK(down == std::vector<int>(p.view.begin(), p.view.end()));
    std::vector<int> up = walk(p, last_visible(p), -1);
    std::reverse(up.begin(), up.end());
    CHECK(up == std::vector<int>(p.view.begin(), p.view.end()));

    // A cursor left on a hidden entry still steps to a visible neighbour.
    int hidden = idx_of(p, "gamma.log");
    CHECK(hidden >= 0);
    CHECK(row_visible(p, hidden) == false);
    CHECK(nav_step(p, hidden, 1) >= 0 || nav_step(p, hidden, -1) >= 0);

    // Page steps stay inside the visible rows.
    int pg = nav_page(p, first_visible(p), 1, 2);
    CHECK(pg < 0 || row_visible(p, pg));
    CHECK(nav_page(p, first_visible(p), -1, 5) == -1);
    CHECK(nav_page(p, last_visible(p), 1, 5) == -1);

    // Range marking while filtered only touches visible rows.
    for (size_t i = 0; i < p.marked.size(); i++) p.marked[i] = false;
    mark_range(p, first_visible(p), last_visible(p));
    for (int i = 0; i < p.entry_count; i++) {
        bool vis = row_visible(p, i);
        bool mk = i < (int)p.marked.size() && p.marked[i];
        if (!vis) CHECK(mk == false);
    }
    CHECK(marked_count(p) == (int)p.view.size() - 1);  // ".." is never marked

    // Clearing the filter restores the full list.
    p.filter[0] = '\0';
    rebuild_view(p);
    CHECK((int)p.view.size() == p.entry_count);
}

static void test_mark_history(Pane& p) {
    p.filter[0] = '\0';
    rebuild_view(p);
    p.mark_undo.clear();
    for (size_t i = 0; i < p.marked.size(); i++) p.marked[i] = false;

    select_all_marks(p);
    CHECK(marked_count(p) == p.entry_count - 1);   // everything but ".."
    restore_marks(p);
    CHECK(marked_count(p) == 0);

    // Two bulk ops stack, so Num/ walks back one step at a time.
    toggle_mark(p, idx_of(p, "alpha.txt"));
    select_all_marks(p);
    invert_marks(p);
    CHECK(marked_count(p) == 0);
    restore_marks(p);
    CHECK(marked_count(p) == p.entry_count - 1);
    restore_marks(p);
    CHECK(marked_count(p) == 1);
}

static void test_step_marks(Pane& p) {
    p.filter[0] = '\0';
    rebuild_view(p);
    for (size_t i = 0; i < p.marked.size(); i++) p.marked[i] = false;

    // Num+ marks the cursor row and steps to the next visible row.
    p.selected = first_visible(p);
    int expect = nav_step(p, p.selected, 1);
    step_marks(p, true);
    CHECK(marked_count(p) <= 1);
    CHECK(p.selected == expect);

    // With a filter, the step skips hidden rows.
    snprintf(p.filter, sizeof(p.filter), ".txt");
    rebuild_view(p);
    int from = idx_of(p, "alpha.txt");
    int nxt = nav_step(p, from, 1);
    CHECK(nxt >= 0 && row_visible(p, nxt));
    p.filter[0] = '\0';
    rebuild_view(p);
}

static void test_clipboard_paths(Pane& p) {
    CHECK(strcmp(path_basename("/a/b/c.txt"), "c.txt") == 0);
    CHECK(strcmp(path_basename("c.txt"), "c.txt") == 0);
    char parent[512];
    path_parent_dir("/a/b/c.txt", parent, sizeof(parent));
    CHECK(strcmp(parent, "/a/b") == 0);

    for (size_t i = 0; i < p.marked.size(); i++) p.marked[i] = false;
    p.selected = idx_of(p, "beta.txt");
    StringList one = target_paths(p);
    CHECK(one.size() == 1);
    CHECK(one.size() == 1 && strstr(one[0].c_str(), "/beta.txt") != nullptr);

    toggle_mark(p, idx_of(p, "alpha.txt"));
    toggle_mark(p, idx_of(p, "gamma.log"));
    StringList many = target_paths(p);
    CHECK(many.size() == 2);   // marks win over the cursor row

    int existing = count_existing_paths(p.current_dir, many);
    CHECK(existing == 2);      // both are real files in this directory
    CHECK(count_existing_paths("/nonexistent-dir-xyz", many) == 0);
}

static void test_refresh_clears_filter(Pane& p) {
    snprintf(p.filter, sizeof(p.filter), ".txt");
    rebuild_view(p);
    CHECK((int)p.view.size() < p.entry_count);

    // A refresh of the same directory keeps the filter (F5/F6/F8 reloads).
    refresh_pane(&p, p.current_dir);
    CHECK(strcmp(p.filter, ".txt") == 0);
    CHECK((int)p.view.size() < p.entry_count);

    // Changing directory drops it, like TC.
    char parent[4200];
    snprintf(parent, sizeof(parent), "%s", p.current_dir);
    char sub[4200];
    snprintf(sub, sizeof(sub), "%s/sub", parent);
    refresh_pane(&p, sub);
    CHECK(p.filter[0] == '\0');
    CHECK((int)p.view.size() == p.entry_count);
    refresh_pane(&p, parent);
}

static void test_tabs(Pane& p, const char* root) {
    refresh_pane(&p, root);
    p.tab_count = 1; p.tab_active = 0;
    snprintf(p.tabs[0].dir, sizeof(p.tabs[0].dir), "%s", root);

    tab_new(&p, true);
    CHECK(p.tab_count == 2);
    CHECK(p.tab_active == 1);

    // Ctrl+Shift+T: appended but not activated.
    tab_new(&p, false);
    CHECK(p.tab_count == 3);
    CHECK(p.tab_active == 1);

    tab_cycle(&p, 1);
    CHECK(p.tab_active == 2);

    // Ctrl+Shift+W: everything but the tab being shown, and it keeps its dir.
    tab_close_all(&p);
    CHECK(p.tab_count == 1);
    CHECK(p.tab_active == 0);
    CHECK(strcmp(p.tabs[0].dir, root) == 0);
    CHECK(strcmp(p.current_dir, root) == 0);

    // The floor is one tab: closing the last one is a no-op.
    tab_close(&p);
    tab_close_all(&p);
    CHECK(p.tab_count == 1);
}

// Ctrl+B branch view: the pane is replaced by the flattened subtree, and every
// row keeps a relative path so operations still address the right file.
static void test_branch_view(AppPanes* app, Pane& p) {
    CHECK(p.list_mode == LIST_NORMAL);
    CHECK(strcmp(entry_rel(p, idx_of(p, "alpha.txt")), "alpha.txt") == 0);

    branch_toggle(app, p, false);
    CHECK(p.list_mode == LIST_BRANCH);
    CHECK((int)p.rel.size() == p.entry_count);
    // a, a/deep, a/deep/x.txt, a/inner.txt, alpha.txt, beta.txt, gamma.log
    CHECK(p.entry_count == 7);
    CHECK(strcmp(entry_rel(p, idx_of(p, "x.txt")), "sub/deep/x.txt") == 0);
    CHECK(strcmp(entry_rel(p, idx_of(p, "sub")), "sub") == 0);
    CHECK((int)p.marked.size() == p.entry_count);
    CHECK((int)p.dir_size.size() == p.entry_count);
    CHECK(idx_of(p, "..") < 0);   // branch view has no parent row

    // The filter works on relative paths in branch view.
    snprintf(p.filter, sizeof(p.filter), "deep");
    rebuild_view(p);
    CHECK((int)p.view.size() == 2);   // sub/deep and sub/deep/x.txt
    p.filter[0] = '\0';
    rebuild_view(p);

    // Ctrl+B again leaves branch view.
    branch_toggle(app, p, false);
    CHECK(p.list_mode == LIST_NORMAL);
    CHECK(p.rel.empty());
    CHECK(p.entry_count == 5);

    // Ctrl+Shift+B walks only the marked directories.
    toggle_mark(p, idx_of(p, "sub"));
    branch_toggle(app, p, true);
    CHECK(p.list_mode == LIST_BRANCH);
    CHECK(p.entry_count == 3);
    CHECK(strcmp(entry_rel(p, idx_of(p, "x.txt")), "sub/deep/x.txt") == 0);
    branch_toggle(app, p, true);
}

// fs_list_tree: depth cap, row cap, dotfiles, and symlinks are not followed.
static void test_fs_list_tree() {
    char tm[] = "/tmp/mc-tree-XXXXXX";
    const char* d = mkdtemp(tm);
    if (!d) { CHECK(false); return; }
    std::string root(d);

    mkdir((root + "/a").c_str(), 0755);
    mkdir((root + "/a/b").c_str(), 0755);
    touch_file(root + "/a/b", "deep.txt");
    touch_file(root, ".dot");
    symlink(root.c_str(), (root + "/loop").c_str());   // a loop: must not be walked

    std::vector<FsTreeEntry> t;
    CHECK(fs_list_tree(root.c_str(), &t, 10, 1000) == 4);   // a, a/b, a/b/deep.txt, loop
    CHECK(strcmp(t[0].rel, "a") == 0);
    CHECK(strcmp(t[1].rel, "a/b") == 0);
    CHECK(strcmp(t[2].rel, "a/b/deep.txt") == 0);
    CHECK(S_ISDIR(t[0].mode));
    CHECK(!S_ISDIR(t[2].mode));

    CHECK(fs_list_tree(root.c_str(), &t, 1, 1000) == 2);    // a, loop
    CHECK(fs_list_tree(root.c_str(), &t, 10, 2) == 2);      // row cap
    CHECK(fs_list_tree(root.c_str(), &t, 0, 1000) == 0);

    fs_set_show_hidden(true);
    CHECK(fs_list_tree(root.c_str(), &t, 10, 1000) == 5);   // + .dot
    fs_set_show_hidden(false);
}

// Phase F primitives: fs_touch never clobbers, fs_symlink stores an absolute
// body, and the in-place copy picks a free ".copy" slot.
static void test_fs_helpers(const std::string& root) {
    CHECK(fs_touch((root + "/fresh.txt").c_str()));
    CHECK(fs_exists((root + "/fresh.txt").c_str()));
    CHECK(!fs_touch((root + "/fresh.txt").c_str()));   // O_EXCL: never truncates

    CHECK(fs_symlink((root + "/alpha.txt").c_str(), (root + "/link.txt").c_str()));
    CHECK(fs_exists((root + "/link.txt").c_str()));
    char buf[512];
    ssize_t n = readlink((root + "/link.txt").c_str(), buf, sizeof(buf) - 1);
    CHECK(n > 0);
    if (n > 0) {
        buf[n] = '\0';
        CHECK(strcmp(buf, (root + "/alpha.txt").c_str()) == 0);
    }
}

static void test_unique_copy_name(const std::string& root) {
    char out[4100];
    unique_copy_name(root.c_str(), "alpha.txt", out, sizeof(out));
    CHECK(strcmp(out, (root + "/alpha.txt.copy").c_str()) == 0);
    CHECK(fs_touch((root + "/alpha.txt.copy").c_str()));
    unique_copy_name(root.c_str(), "alpha.txt", out, sizeof(out));
    CHECK(strcmp(out, (root + "/alpha.txt.copy.1").c_str()) == 0);
    CHECK(fs_touch((root + "/alpha.txt.copy.1").c_str()));
    unique_copy_name(root.c_str(), "alpha.txt", out, sizeof(out));
    CHECK(strcmp(out, (root + "/alpha.txt.copy.2").c_str()) == 0);
}

// The trash half of TC's delete split. Availability is probed once and cached;
// a path no helper can resolve must fail rather than fall through silently.
static void test_trash_helpers(void) {
    bool a = fs_trash_available();
    CHECK(a == fs_trash_available());
    CHECK(!fs_trash("/mc-clone-does-not-exist-xyzzy"));
}

// TC quick search: prefix match from the cursor forward, wrapping, extending
// the word, and respecting the filtered view. A miss leaves the cursor alone.
static void test_quick_search(AppPanes* app, Pane& p) {
    p.quick_active = false; p.quick[0] = '\0';
    p.selected = idx_of(p, "alpha.txt");
    quick_search(app, p, 'b');
    CHECK(p.quick_active);
    CHECK(strcmp(p.quick, "b") == 0);
    CHECK(strcmp(entry_rel(p, p.selected), "beta.txt") == 0);

    quick_search(app, p, 'x');                 // "bx": no match, cursor stays
    CHECK(strcmp(p.quick, "bx") == 0);
    CHECK(strcmp(entry_rel(p, p.selected), "beta.txt") == 0);

    p.quick_active = false; p.quick[0] = '\0';
    p.selected = idx_of(p, "beta.txt");
    quick_search(app, p, 'a');                 // wraps past the end back to alpha
    CHECK(strcmp(entry_rel(p, p.selected), "alpha.txt") == 0);

    p.quick_active = false; p.quick[0] = '\0';
    p.selected = idx_of(p, "alpha.txt");
    snprintf(p.filter, sizeof(p.filter), "sub");
    rebuild_view(p);
    quick_search(app, p, 's');                 // only visible rows are searched
    CHECK(strcmp(entry_rel(p, p.selected), "sub") == 0);
    quick_search(app, p, 'u');                 // "su" extends the word
    CHECK(strcmp(entry_rel(p, p.selected), "sub") == 0);
    p.filter[0] = '\0';
    rebuild_view(p);
    p.quick_active = false; p.quick[0] = '\0';
}

// Shift+F1 column set: parsing tolerates case and padding, unknown keys are
// dropped, and the render cache must notice a rewritten spec.
static void test_columns(AppPanes* app) {
    bool on[COL_N];
    columns_parse("name,size,date,perms", on);
    CHECK(on[COL_NAME] && on[COL_SIZE] && on[COL_DATE] && on[COL_PERMS]);
    CHECK(!on[COL_EXT] && !on[COL_TYPE]);

    columns_parse("name, ext , TYPE", on);
    CHECK(on[COL_EXT] && on[COL_TYPE] && !on[COL_SIZE]);

    columns_parse("name,name,bogus", on);
    CHECK(on[COL_NAME]);
    CHECK(!on[COL_EXT] && !on[COL_SIZE] && !on[COL_DATE] && !on[COL_PERMS] && !on[COL_TYPE]);

    for (int c = 0; c < COL_N; c++) on[c] = (c == COL_NAME || c == COL_EXT || c == COL_TYPE);
    columns_write(app, on);
    CHECK(strcmp(app->columns, "name,ext,type") == 0);
    columns_spec(app, on);
    CHECK(on[COL_EXT] && on[COL_TYPE] && !on[COL_SIZE]);

    CHECK(strcmp(col_ext("archive.tar.gz"), "gz") == 0);
    CHECK(strcmp(col_ext("Makefile"), "") == 0);
    CHECK(strcmp(col_ext(".hidden"), "") == 0);      // no extension on dotfiles
    CHECK(strcmp(col_ext("README."), "") == 0);

    FileEntry fe;
    memset(&fe, 0, sizeof(fe));
    fe.mode = S_IFDIR | 0755;
    CHECK(strcmp(col_type(&fe, true, false), "DIR") == 0);
    fe.mode = S_IFLNK | 0777;
    CHECK(strcmp(col_type(&fe, false, false), "LNK") == 0);
    fe.mode = S_IFREG | 0755;
    CHECK(strcmp(col_type(&fe, false, true), "EXE") == 0);
    fe.mode = S_IFREG | 0644;
    CHECK(strcmp(col_type(&fe, false, false), "FILE") == 0);

    snprintf(app->columns, sizeof(app->columns), "name,size,date,perms");
}

// TC tree panel: the model is per pane, expand/collapse is symmetric, and the
// arrow keys drive it without touching the file rows.
static void test_tree_panel(AppPanes* app, Pane& p, const std::string& root) {
    snprintf(p.current_dir, sizeof(p.current_dir), "%s", root.c_str());
    tree_fill(p);
    CHECK(strcmp(p.tree_rows[0].path, root.c_str()) == 0);
    CHECK(p.tree_rows[0].depth == 0 && p.tree_rows[0].expanded);
    CHECK(p.tree_rows.size() >= 2);            // root + sub

    int sub = -1;
    const std::string subpath = root + "/sub";
    for (int i = 1; i < (int)p.tree_rows.size(); i++)
        if (p.tree_rows[i].path == subpath) sub = i;
    CHECK(sub >= 0);

    size_t before = p.tree_rows.size();
    tree_toggle(p, sub);                        // sub/deep splices in
    CHECK(p.tree_rows.size() == before + 1);
    CHECK(p.tree_rows[sub + 1].depth == p.tree_rows[sub].depth + 1);
    tree_toggle(p, sub);                        // and comes back out symmetrically
    CHECK(p.tree_rows.size() == before);

    // Each pane owns its model: expanding in one must not move the other.
    snprintf(app->right.current_dir, sizeof(app->right.current_dir), "%s", root.c_str());
    app->right.tree_rows.clear();
    tree_fill(app->right);
    CHECK(app->right.tree_rows.size() == p.tree_rows.size());
    tree_toggle(app->right, sub);
    CHECK(app->right.tree_rows.size() == p.tree_rows.size() + 1);
    app->right.tree_rows.clear();

    p.tree_selected = 0;
    CHECK(tree_nav(app, p, SDLK_DOWN));
    CHECK(p.tree_selected == 1);
    CHECK(tree_nav(app, p, SDLK_UP));
    CHECK(p.tree_selected == 0);
    CHECK(tree_nav(app, p, SDLK_UP));           // clamped at the top
    CHECK(p.tree_selected == 0);
    CHECK(tree_nav(app, p, SDLK_END));
    int end = p.tree_selected;
    CHECK(end == (int)p.tree_rows.size() - 1);
    CHECK(tree_nav(app, p, SDLK_LEFT));         // collapse, or jump to the parent
    CHECK(p.tree_selected <= end);
    CHECK(!tree_nav(app, p, SDLK_F3));          // unrelated keys fall through

    tree_mode_toggle(app, p, TREE_EMBED);
    CHECK(p.tree_mode == TREE_EMBED);
    tree_mode_toggle(app, p, TREE_EMBED);       // same mode again turns it off
    CHECK(p.tree_mode == TREE_OFF);
    tree_mode_toggle(app, p, TREE_SPLIT);
    CHECK(p.tree_mode == TREE_SPLIT);
    tree_mode_toggle(app, p, TREE_EMBED);       // switching kinds, not off
    CHECK(p.tree_mode == TREE_EMBED);
    p.tree_mode = TREE_OFF;
}

// TC's archive keys must stay reserved. Alt+F5 leaking into plain F5 is what
// made the app copy files when the user asked it to pack them.
static void test_archive_keys(void) {
    CHECK(is_archive_key(SDLK_F5, SDL_KMOD_ALT));
    CHECK(is_archive_key(SDLK_F5, SDL_KMOD_ALT | SDL_KMOD_SHIFT));
    CHECK(is_archive_key(SDLK_F6, SDL_KMOD_ALT));
    CHECK(is_archive_key(SDLK_F9, SDL_KMOD_ALT));
    CHECK(is_archive_key(SDLK_F9, SDL_KMOD_ALT | SDL_KMOD_SHIFT));
    // plain and Ctrl forms belong to copy / move / the pane menu
    CHECK(!is_archive_key(SDLK_F5, SDL_KMOD_NONE));
    CHECK(!is_archive_key(SDLK_F5, SDL_KMOD_SHIFT));
    CHECK(!is_archive_key(SDLK_F6, SDL_KMOD_NONE));
    CHECK(!is_archive_key(SDLK_F9, SDL_KMOD_NONE));
    CHECK(!is_archive_key(SDLK_F5, SDL_KMOD_CTRL));
    // neighbouring Alt keys are real features, not archive keys
    CHECK(!is_archive_key(SDLK_F4, SDL_KMOD_ALT));
    CHECK(!is_archive_key(SDLK_F7, SDL_KMOD_ALT));
    CHECK(!is_archive_key(SDLK_F8, SDL_KMOD_ALT));
    CHECK(!is_archive_key(SDLK_F10, SDL_KMOD_ALT));
}

// Drive the real key handler, not just the classifier: this is the path that
// regressed, where Alt+F5 reached the plain F5 handler and copied files.
static void test_archive_keys_live(AppPanes* app, const std::string& root) {
    Pane& p = app->left;
    app->left_focus = true;
    refresh_pane(&p, root.c_str());
    int before = p.entry_count;

    int ai = idx_of(p, "alpha.txt");
    CHECK(ai >= 0);
    p.selected = ai;
    p.marked[ai] = true;

    app_panes_handle_key(app, SDLK_F5, SDL_KMOD_ALT);
    CHECK(strstr(app->status_msg, "Pack") != NULL);
    CHECK(strstr(app->status_msg, "not supported") != NULL);
    CHECK(!app->prog_active);
    CHECK(app->pending_op == 0);
    CHECK(app->clipboard.paths.empty());      // nothing queued for a copy

    app_panes_handle_key(app, SDLK_F5, SDL_KMOD_ALT | SDL_KMOD_SHIFT);
    CHECK(strstr(app->status_msg, "Pack and delete") != NULL);
    CHECK(fs_exists((root + "/alpha.txt").c_str()));   // not deleted

    app_panes_handle_key(app, SDLK_F6, SDL_KMOD_ALT);
    CHECK(strstr(app->status_msg, "Extract") != NULL);
    CHECK(!app->prog_active);

    app->pane_menu_open = false;
    app_panes_handle_key(app, SDLK_F9, SDL_KMOD_ALT);
    CHECK(!app->pane_menu_open);              // the pane menu stays shut
    CHECK(strstr(app->status_msg, "Extract") != NULL);

    app->pane_menu_open = false;
    app_panes_handle_key(app, SDLK_F9, SDL_KMOD_ALT | SDL_KMOD_SHIFT);
    CHECK(!app->pane_menu_open);
    CHECK(strstr(app->status_msg, "Test archive") != NULL);

    refresh_pane(&p, root.c_str());
    CHECK(p.entry_count == before);           // no copies, no .copy files, no moves
    CHECK(fs_exists((root + "/alpha.txt").c_str()));

    // the unmodified key still opens the pane menu
    app_panes_handle_key(app, SDLK_F9, SDL_KMOD_NONE);
    CHECK(app->pane_menu_open);
    app->pane_menu_open = false;

    free_entries(p.entries, p.entry_count);
    p.entries = nullptr;
    p.entry_count = 0;
}

int main() {
    char tmpl[] = "/tmp/mc-clone-test-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { printf("cannot create temp dir\n"); return 1; }
    std::string root(dir);

    touch_file(root, "alpha.txt");
    touch_file(root, "beta.txt");
    touch_file(root, "gamma.log");
    mkdir((root + "/sub").c_str(), 0755);
    touch_file(root + "/sub", "inner.txt");
    mkdir((root + "/sub/deep").c_str(), 0755);
    touch_file(root + "/sub/deep", "x.txt");
    touch_file(root, ".hidden");

    fs_set_sort_mode(FS_SORT_NAME);
    fs_set_show_hidden(false);

    Pane p;
    p.entries = nullptr;
    p.entry_count = 0;
    p.selected = 0;
    p.anchor = 0;
    p.scroll_request = false;
    p.current_dir[0] = '\0';

    AppPanes app;

    test_matcher();

    refresh_pane(&p, root.c_str());
    CHECK(p.entry_count == 5);   // ..  alpha.txt  beta.txt  gamma.log  sub
    CHECK((int)p.marked.size() == p.entry_count);
    CHECK((int)p.dir_size.size() == p.entry_count);
    CHECK(p.dir_size[idx_of(p, "sub")] == -1);

    test_view_and_nav(p);
    test_mark_history(p);
    test_step_marks(p);

    refresh_pane(&p, root.c_str());
    test_clipboard_paths(p);
    test_refresh_clears_filter(p);
    test_tabs(p, root.c_str());
    test_branch_view(&app, p);
    test_fs_list_tree();
    test_fs_helpers(root);
    test_unique_copy_name(root);
    test_trash_helpers();
    test_quick_search(&app, p);
    test_columns(&app);
    test_tree_panel(&app, p, root);
    test_archive_keys();
    test_archive_keys_live(&app, root);

    free_entries(p.entries, p.entry_count);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
