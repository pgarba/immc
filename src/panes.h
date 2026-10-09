#ifndef PANES_H
#define PANES_H

#include <stdbool.h>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include "filesystem.h"

static const int MAX_TABS = 8;
static const int MAX_DIR_HISTORY = 32;
static const int MAX_SEL_MEMORY = 64;

typedef struct {
    char dir[4096];
} PaneTab;

typedef struct {
    char dir[4096];
    char name[256];
} PaneSelMemory;

// A full set of mark flags, parallel to a pane's entries. Named so the mark
// history below does not need a nested "vector<vector<bool>>".
using MarkSet = std::vector<bool>;
// A list of absolute paths.
using StringList = std::vector<std::string>;

// Pane listing mode: the plain directory, TC's branch view (Ctrl+B), which
// flattens the whole subtree below the pane's directory into one list, or an
// archive's contents. Archive browsing is a virtual listing: current_dir holds
// a path whose prefix is the archive file (see arch_split_virtual), so tabs,
// history and the path bar carry it without knowing archives exist.
enum PaneListMode { LIST_NORMAL = 0, LIST_BRANCH = 1, LIST_ARCHIVE = 2 };

// TC puts a directory tree inside a panel: Ctrl+Shift+F8 replaces the file
// list with it, Shift+Tab splits the panel into tree + list.
enum PaneTreeMode { TREE_OFF = 0, TREE_EMBED = 1, TREE_SPLIT = 2 };

// One visible row of a directory tree. Collapsed subtrees simply are not in
// the list, so the vector is always the set of rows on screen.
struct TreeRow {
    char path[4096];
    int  depth;
    bool expanded;
    bool has_children;
};
using TreeRows = std::vector<TreeRow>;

struct Pane {
    char current_dir[4096];
    FileEntry** entries;
    int entry_count;
    int selected;
    int anchor;          // last click / selection start for shift+click ranges
    bool scroll_request; // set when keyboard moves the selection; render scrolls to it
    char path_edit[4096]; // edit buffer for the path bar
    bool path_edit_active; // true while the path InputText has keyboard focus
    bool path_focus_request; // one-shot: focus the path bar next frame (Alt+F11/F12)
    std::vector<bool> marked; // multi-select flags, parallel to entries
    std::vector<long long> dir_size; // TC Space on a dir: summed size, -1 = unknown
    std::vector<MarkSet> mark_undo;  // TC Num/: mark states to restore
    // TC quick filter. `view` maps visible rows onto indices in `entries`; the
    // filter row is edited by ImGui, so filter_applied detects the change.
    char filter[256];
    char last_filter[256];      // Ctrl+Shift+S re-applies this
    char filter_applied[256];   // filter text the current `view` was built from
    bool filter_active;         // filter row is shown
    bool filter_field_active;   // the row holds the keyboard this frame
    bool filter_focus_request;  // one-shot: focus the row next frame
    bool filter_exec_only;      // Ctrl+F11: only executables pass the filter
    // TC quick search (AltGr + letter): the word typed so far. AltGr arrives as
    // SDL_KMOD_MODE, or as the right Alt on layouts without a real AltGr.
    char quick[128];
    bool quick_active;
    std::vector<int> view;      // visible rows -> entry index
    bool quick_view;          // F3 quick view for this panel
    int list_mode = LIST_NORMAL;  // LIST_NORMAL | LIST_BRANCH
    // TC tree panel (Ctrl+Shift+F8 embeds it, Shift+Tab splits the panel).
    // Each pane owns its model: the floating Alt+F10 window edits the target
    // pane's model too, so both views stay in sync.
    int tree_mode = TREE_OFF;   // TREE_OFF | TREE_EMBED | TREE_SPLIT
    TreeRows tree_rows;
    int tree_selected = 0;      // cursor row inside tree_rows
    StringList rel;           // branch view: path of each row under current_dir
                              // archive view: path of each member inside the archive
    int list_gen;             // bumped on every reload; caches key off this
    float cursor_screen_x;    // screen pos of the cursor row, for Shift+F10
    float cursor_screen_y;
    int page_size;            // visible rows, measured during render (0 = fallback)

    // Archive the pane is browsing, when it is inside one: the real path of the
    // archive file plus the member directory currently shown ("" = top level).
    char archive_path[4096];
    char archive_prefix[1024];

    // Directory history (Alt+Left / Alt+Right)
    char history[MAX_DIR_HISTORY][4096];
    int history_count; // number of valid slots
    int history_pos;   // index of the current dir within history

    // Per-directory selection memory: remembers the selected entry name per
    // directory so re-entering it (enter a dir, then go back) restores the
    // cursor position.
    PaneSelMemory sel_memory[MAX_SEL_MEMORY];
    int sel_mem_count;

    // Tabs (Ctrl+T new, Ctrl+W close, Ctrl+1..9 switch)
    PaneTab tabs[MAX_TABS];
    int tab_count;
    int tab_active;
};

// Pending file operation shown in the bottom op bar
enum AppOp {
    OP_NONE = 0,
    OP_RENAME,   // op_name = new name for the target entry
    OP_MKDIR,    // op_name = new directory name
    OP_NEWFILE   // op_name = name of an empty file to create
};

// An action a widget asked for. The widget fires inside the ImGui frame, but
// these operations block and pump their own progress UI, which opens a new
// ImGui frame; doing that with a frame already open corrupts ImGui. So the
// widget records the request and app_panes_run_pending() carries it out between
// frames. Anything that can reach pump_progress_ui() belongs here.
enum AppDeferred {
    DEFER_NONE = 0,
    DEFER_COPY,
    DEFER_MOVE,
    DEFER_PASTE,
    DEFER_EXTRACT,        // Alt+F6 from the Commands menu
    DEFER_PACK,           // the pack dialog's Pack button
    DEFER_COPY_IN_PLACE,  // Shift+F5
    DEFER_SYMLINK_OTHER,  // Shift+Ctrl+F5
    DEFER_FILL_SIZES      // Alt+Shift+Enter
};

// Pending confirmation dialog (Y/N)
enum AppConfirm {
    CONF_NONE = 0,
    CONF_DELETE,     // delete marked/current entries from active pane
    CONF_OVERWRITE,  // copy (F5) targets that already exist
    CONF_OVERWRITE_MOVE, // move (F6) targets that already exist
    CONF_OVERWRITE_PASTE, // clipboard paste (Ctrl+V) targets that already exist
    CONF_OVERWRITE_EXTRACT // copy (F5) out of an archive onto existing files
};

// Internal file clipboard (TC Ctrl+C / Ctrl+X / Ctrl+V). Absolute source paths
// are stored rather than pane indices so the source pane can change meanwhile.
// `archive` is non-empty when the sources are archive members: `paths` then
// holds member paths inside that archive rather than filesystem paths, and
// pasting extracts instead of copying. A cut cannot name a member, because
// there is no way to delete one.
typedef struct {
    bool cut;   // true = move on paste, false = copy
    StringList paths;
    char archive[4096];
} FileClipboard;

// Panel view modes (Ctrl+O cycles): MC-style brief / medium / full
enum AppViewMode {
    VIEW_MID = 0,  // name + size + date
    VIEW_BRIEF,    // two name columns
    VIEW_FULL      // name + size + date + perms
};

class AppPanes {
public:
    Pane left;
    Pane right;
    bool left_focus;
    int active_path_pane; // -1 = none, 0 = left, 1 = right (path InputText has focus)

    AppOp op = OP_NONE;
    char op_name[256];    // name input for rename/mkdir
    bool op_input_active; // true while the op bar InputText has focus
    char status_msg[512]; // last operation result, shown in the status bar

    // Command line (bottom bar). Focused by default; special keys are routed
    // to the panes, printable keys type into it.
    char cmd[512];
    char cmd_history[64][512];
    int cmd_hist_count;
    int cmd_hist_pos; // == cmd_hist_count means "editing a fresh line"

    // Command output window (result of the last executed command)
    bool output_open;
    char output_text[64 * 1024];

    // Confirmation dialog
    AppConfirm confirm;
    char confirm_text[512];

    // Hotlist (bookmarks)
    std::vector<std::string> hotlist;
    bool hotlist_open;
    bool dirhist_open;    // Alt+Down: visited directories
    bool cmdhist_open;    // Alt+F8: command line history
    bool volumes_open;    // Alt+F1/F2: mounted drives
    int  volumes_target;  // pane index the volumes list navigates

    // Internal file clipboard (Ctrl+C / Ctrl+X / Ctrl+V)
    FileClipboard clipboard;

    // Search window (Shift+F7)
    bool search_open;
    char search_pattern[256];
    std::vector<std::string> search_results; // full paths of matches
    bool search_done;
    bool search_input_active; // pattern field has keyboard focus

    // Full file viewer (Enter on a plain file)
    bool viewer_open;
    char viewer_path[4096];
    // Set when the viewer is showing an archive member: the temporary directory
    // it was unpacked into, removed when the viewer closes.
    char viewer_tmp_dir[4096];

    // Properties dialog (Alt+Enter): recursive size/count of one entry
    bool props_open;
    char props_path[4096];
    long long props_size;
    long props_files;
    long props_dirs;

    // Copy/move progress dialog (the UI is pumped during the operation)
    bool      prog_active;
    bool      prog_is_move;
    bool      prog_is_delete;
    char      prog_src[4100];
    char      prog_dst[4100];
    long long prog_done;
    long long prog_total;
    int       prog_files_done;
    int       prog_files_total;
    double    prog_t0;
    double    prog_last_pump;

    // Deferred actions: menu/context-menu/dialog-button triggers fire inside
    // the ImGui frame, but a long operation pumps its own progress UI, and that
    // starts a new ImGui frame. Starting one while a frame is open corrupts
    // ImGui, so these run between frames instead.
    int  pending_op;          // AppDeferred
    bool pending_confirm_yes;

    // Pack dialog (TC Alt+F5). Packing has real choices - format, level, a
    // password, volumes - so it gets a dialog rather than a name in the op bar.
    // The sources are captured when it opens so marks cannot change underneath
    // it, and the target directory is remembered so the dialog can be opened
    // while the pane is showing an unrelated tab.
    bool pack_open;
    char pack_name[512];           // without the suffix; the format supplies it
    char pack_dir[4096];           // directory the archive is written into
    int  pack_kind_idx;            // index into arch_pack_kinds()
    int  pack_level;               // 0 store .. 9 best
    bool pack_use_level;           // false = the tool's own default
    char pack_password[256];       // empty = no encryption
    long long pack_volume_bytes;   // 0 = one file; only 7z can split
    bool pack_delete;              // remove the sources once packed
    StringList pack_names;         // sources, relative to pack_dir

    void* sdl_window;         // SDL_Window*, for pumping the progress UI

    bool about_open;     // Help > About

    // Keyboard-opened menus (TC F10 = app menu, F9 = menu above the pane).
    // *_open is a one-shot open request consumed by render(); *_alive is
    // maintained by render() so the key handler can close a live popup.
    bool menu_open, menu_alive;
    bool pane_menu_open, pane_menu_alive;
    bool entry_menu_open;     // Shift+F10: context menu at the cursor row
    bool ctx_menu_alive;      // the context popup is open this frame
    bool compare_on;          // Shift+F2: color rows against the other pane
    int  trash_mode;          // config: Del/F8 goes to the trash
    int  delete_trash;        // set by the delete prompt: this delete goes to trash
    bool quick_view_pane;     // Ctrl+Q: the inactive pane previews the cursor file
    bool tree_open;           // Alt+F10 / Ctrl+F8: directory tree window
    int  tree_target;         // pane index the tree navigates
    bool popup_close_request;

    int view_mode;      // AppViewMode, Ctrl+O cycles
    bool show_hidden;   // Alt+. toggles
    char filter_preset[256];  // Ctrl+F12 user filter, from the config
    // Shift+F1 column set for the long view: comma-separated, always starting
    // with "name". Vocabulary: name, ext, size, date, perms, type.
    char columns[256];
    bool columns_open;          // the "Define columns" dialog is up
    bool quit_request;  // F10 / Alt+Q / menu Quit

    AppPanes();
    void render();
    void refresh_pane(Pane& pane);
};

// C-style interface functions
AppPanes* app_panes_init();
void app_panes_render(AppPanes* app);
void app_panes_free(AppPanes* app);
void app_panes_handle_key(AppPanes* app, SDL_Keycode key, SDL_Keymod mod);
bool app_key_is_special(SDL_Keycode key, SDL_Keymod mod, AppPanes* app);
void app_panes_commit_path(AppPanes* app); // navigate to the typed path in the focused path bar
void app_panes_commit_op(AppPanes* app);   // execute the pending op (rename/mkdir) from the op bar
void app_panes_cancel_op(AppPanes* app);   // cancel a pending op
void app_panes_run_pending(AppPanes* app); // run deferred menu/dialog actions
void app_panes_set_sdl_window(AppPanes* app, void* window);

struct MCConfig;
void app_panes_save_state(AppPanes* app, MCConfig* cfg);          // tabs/dirs -> config
void app_panes_apply_state(AppPanes* app, const MCConfig* cfg);   // config -> tabs/dirs

#endif // PANES_H
