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

    free_entries(p.entries, p.entry_count);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
