#ifndef PANES_H
#define PANES_H

#include <stdbool.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "filesystem.h"
#include "remote.h"   // NetSite, for the site list AppPanes owns

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
// flattens the whole subtree below the pane's directory into one list, an
// archive's contents, or a directory on a remote site. The last three are
// virtual listings: current_dir holds a path that only this program understands
// (see arch_split_virtual and net_split_virtual), so tabs, history and the path
// bar carry it without knowing archives or FTP exist, and the operations that
// talk to the filesystem have to step back.
enum PaneListMode { LIST_NORMAL = 0, LIST_BRANCH = 1, LIST_ARCHIVE = 2,
                    LIST_REMOTE = 3 };

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
    // Set while browsing a site: the site root URL (scheme, user, host, port -
    // never a password) and the path on the server. current_dir holds the
    // virtual path built from the two, which is what lets tabs, history, the
    // path bar and the hotlist carry a remote directory without knowing that
    // FTP exists. The login itself is looked up by root URL, so a pane never
    // holds a secret a screenshot or a saved session could leak.
    char remote_root[512];
    char remote_path[1024];
    // The local directory the pane walked into the site from. ".." at the site
    // root uses it to leave, because a remote pane has no parent of its own.
    char remote_came_from[4096];

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

// A file operation the op dialog decided, carried out by run_op_plan(). The
// sources are captured as names relative to `src_dir` - or as member paths when
// `archive` is non-empty - instead of being re-read from the pane's marks at run
// time, so what the dialog showed is what runs even if a click changes the pane
// while a confirm dialog is up.
struct OpPlan {
    std::string src_dir;         // real directory the source names resolve against
    StringList names;            // sources, relative to src_dir (members inside archive)
    StringList raw_names;        // archive only: those members as the tool named them
    std::string dst_dir;         // destination directory
    std::string name_override;   // non-empty: the one source lands under this name
    std::string archive;         // non-empty: the sources are members of this archive
    std::string archive_prefix;  // level inside that archive, when the pane was deeper
    std::string remote;          // non-empty: the sources are entries on this site
    std::string remote_prefix;   // level inside that site, when the pane was deeper
    bool is_move = false;
};

// A copy or move across a network that runs on its own thread instead of
// blocking the panes. The counters are written by the worker and read by the
// frame that draws the transfer list, so they are atomic. The record itself
// belongs to the UI thread, which creates it, starts the worker, draws it and
// joins the worker before erasing the record - a std::thread that is still
// joinable when it is destroyed ends the process. `error` and `ok_names` are the
// worker's: it writes them and then stores the finishing state, and a frame that
// reads that state is allowed to read them.
struct BgTransfer {
    enum State { BG_RUNNING = 0, BG_DONE, BG_FAILED, BG_ABORTED };

    std::atomic<int>       state;       // one of State
    std::atomic<bool>      abort;       // the UI's request; the worker polls it
    std::atomic<bool>      settled;     // the finish has been handled once already
    std::atomic<bool>      dismiss;     // the row's button: retire it next frame
    std::atomic<long long> done;        // bytes moved so far
    std::atomic<long long> total;       // bytes the transfer knows it owes
    std::atomic<int>       files_done;
    std::atomic<int>       files_total;

    // Fixed before the worker starts; the worker only reads them.
    bool   is_move;
    bool   download;        // true: server -> disk, false: disk -> server
    double t0;              // SDL_GetTicks() seconds at the start
    std::string src, dst;

    // The worker's answer. Read once state is no longer BG_RUNNING.
    std::string error;
    int ok_names = 0;

    std::thread worker;
};

// The preview a quick view shows when the file is not on this disk: a member of
// an archive, or a file on a site. The cursor moves with the arrow keys and a
// fetch over a network takes longer than the frame that asked for it, so the
// fetch runs on a thread and the answer lands in a later frame. A fetch carries
// the path it was asked for, because by the time it answers the cursor is
// usually somewhere else and the answer is only good for that path. Same
// ownership rule as BgTransfer: the UI thread joins the worker before the record
// goes away, and only ever joins one that has already finished.
struct QvFetch {
    std::atomic<bool> cancel{false};   // nobody is looking; stop early if you can
    std::atomic<bool> done{false};     // text and err are readable by the UI thread
    // Set by a quick view that asked for this key in the frame that just drew,
    // and cleared again by qv_reap() at the head of the next frame. Only the UI
    // thread touches it, which is why it is not atomic the way cancel is.
    bool wanted = false;
    std::string key;                   // the path this is a preview of
    std::string text;                  // the worker's answer, written before done
    std::string err;
    std::thread worker;
};

// Pending file operation, shown as a dialog with an editable destination
enum AppOp {
    OP_NONE = 0,
    OP_RENAME,   // op_name = new name for the target entry
    OP_MKDIR,    // op_name = new directory name
    OP_NEWFILE,  // op_name = name of an empty file to create
    OP_COPY,     // op_name = destination, sources in op_plan
    OP_MOVE      // as OP_COPY, moving instead of copying
};

// An action a widget asked for. The widget fires inside the ImGui frame, but
// these operations block and pump their own progress UI, which opens a new
// ImGui frame; doing that with a frame already open corrupts ImGui. So the
// widget records the request and app_panes_run_pending() carries it out between
// frames. Anything that can reach pump_progress_ui() belongs here.
enum AppDeferred {
    DEFER_NONE = 0,
    DEFER_COPY,           // F5 / the menu item: opens the copy dialog
    DEFER_MOVE,           // F6 / the menu item: opens the move dialog
    DEFER_OP_COMMIT,        // the op dialog's Enter or OK button
    DEFER_OP_RUN,         // run the plan the op dialog settled on
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
    char op_name[4096];   // the dialog's field: a name, or a destination path
    OpPlan op_plan;       // sources and resolved destination for OP_COPY/OP_MOVE
    bool op_focus_request;   // focus the dialog's field on the frame it opens
    char op_error[512];   // the dialog's own error line, cleared on open
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
    // Remote sites. `remote_sites` mirrors the sites file; `remote_adhoc` holds
    // the quick connects made this session, which are never written back. Both
    // are consulted whenever a pane navigates to a remote path, because a
    // virtual path carries no password to log in with.
    std::vector<NetSite> remote_sites;
    std::vector<NetSite> remote_adhoc;
    bool remote_mgr_open;      // Ctrl+F: the saved-site manager
    bool remote_quick_open;    // Ctrl+N: one-line quick connect
    char remote_quick[512];    // the quick-connect field
    char remote_note[512];     // the last connect or listing failure
    int  remote_edit;          // index into remote_sites being edited; -1 = add one
    // Which connection the arrow keys have highlighted in the manager. The list
    // it walks is the saved sites followed by this session's quick connects, so
    // the site you reached with Ctrl+N a moment ago is one keypress from a
    // keyboard. -1 = nothing highlighted.
    int  remote_cursor;
    bool remote_focus_request; // the quick-connect field wants the caret
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
    // Set by the progress dialog's Abort button. The copy and the transport both
    // poll the progress callback, which returns false while this is set, so the
    // stop lands between files rather than in the middle of one.
    bool      prog_abort;
    // The transfer the progress dialog is watching. A copy that crosses a network
    // runs on its own thread whether or not the user asked for the background, so
    // the dialog is a window onto a row in `transfers` rather than the thing
    // driving the copy - which is what lets it be handed over mid-file. Null for
    // an operation running on this thread, where there is nothing to hand over.
    BgTransfer* prog_watch;
    // The progress dialog's "Run in background" button: stop watching, leave the
    // transfer running, and let the list under the panes carry it from here.
    bool      prog_handoff;

    // Copy/move transfers running on their own thread instead of blocking the
    // panes, oldest first. The frame draws them; bg_reap() settles the finished
    // ones (join, refresh, status line) and retires the ones the user dismissed.
    // app_panes_free() asks the running ones to stop and joins them before the
    // app goes away.
    std::vector<std::unique_ptr<BgTransfer>> transfers;
    // Quick view. `qv_cache` is the text already read, keyed by the path the pane
    // shows, so moving the cursor back onto a name costs nothing; `qv_fetches`
    // are the previews that are not on this disk and are still on their way.
    std::vector<std::pair<std::string, std::string>> qv_cache;
    std::vector<std::unique_ptr<QvFetch>> qv_fetches;
    // The op dialog's "Run in background" request. It lives here rather than in
    // the plan because it has to survive the overwrite confirm, and only
    // run_op_plan() knows whether the plan crosses a network at all - which is
    // the only kind of transfer that can be handed to a thread.
    bool      op_run_bg;

    // Deferred actions: menu/context-menu/dialog-button triggers fire inside
    // the ImGui frame, but a long operation pumps its own progress UI, and that
    // starts a new ImGui frame. Starting one while a frame is open corrupts
    // ImGui, so these run between frames instead.
    int  pending_op;          // AppDeferred
    bool pending_confirm_yes;

    // Pack dialog (TC Alt+F5). Packing has real choices - format, level, a
    // password, volumes - so it gets a dialog rather than one line in the
    // bottom bar.
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

    // Options dialog (Options > Fonts and theme...). These mirror the config, so
    // whatever the dialog changed is what gets written back on exit. The dialog
    // applies them live, and a font atlas cannot be rebuilt while a frame is
    // open, so a font pick sets font_dirty and main.cpp applies it between
    // frames, next to the other things that cannot run inside one.
    bool  options_open;
    bool  options_field_active; // the font filter holds the keyboard this frame
    // ImGui keeps one keyboard-focus request per frame and the last one wins, so
    // whoever asks first has to say so: the command line at the bottom of the
    // window asks on every frame. Transient, never saved.
    bool  focus_claimed;
    char  theme[64];        // a name from mc_theme_list()
    char  font[1024];       // path to a .ttf/.otf; empty = the built-in font
    float font_size;        // px; 0 = the built-in default size
    char  hinting[32];      // a name from mc_hinting_list(); how glyphs meet the pixel grid
    float display_scale;    // device pixels per logical pixel; measured, not saved
    bool  font_dirty;       // a font pick is waiting to be applied
    char  font_msg[256];    // the dialog's line about the last pick
    StringList font_files;  // scanned font files, in picker order
    bool  font_scanned;     // font_files is filled in
    char  font_filter[128]; // narrows the picker list

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
void app_panes_commit_op(AppPanes* app);   // carry out the op the dialog is showing
void app_panes_cancel_op(AppPanes* app);   // cancel a pending op
void app_panes_run_pending(AppPanes* app); // run deferred menu/dialog actions
void app_panes_set_sdl_window(AppPanes* app, void* window);
// Tell the panes the window's framebuffer scale. Returns true when it changed,
// which means the glyphs are being drawn at the wrong density and the atlas has
// to be rebuilt - the caller does that between frames, as it does a font pick.
bool app_panes_set_display_scale(AppPanes* app, float scale);

// Identity and title bar. Plain functions over buffers so the strings the user
// judges are identity-critical can be checked without a display.
void app_identity_line(char* out, size_t n);   // "immc 0.9.0 (mc-clone)"
void app_about_identity(char* out, size_t n);  // the About window's header block
void app_window_title(AppPanes* app, char* out, size_t n); // "immc - <active dir>"

// Fonts. app_font_apply rebuilds the atlas from `path`/`size`/`hinting` and
// returns false when the file was rejected, having already put the built-in font
// back so the app is never left without text. app_scan_fonts collects what this
// machine has.
bool app_font_apply(const char* path, float size, const char* hinting);
void app_panes_apply_font(AppPanes* app);      // carry out a pending dialog pick
void app_scan_fonts(StringList& out);

// Hinting modes, by name because the config stores names, the same way the theme
// is stored. mc_font_hinting_available() is false in a build without FreeType,
// which has no hinting to offer - stb_truetype does none at all.
const char* const* mc_hinting_list(int* count);
int  mc_hinting_index(const char* name);       // -1 when the name is unknown
bool mc_font_hinting_available();
void mc_font_rasterizer(char* out, size_t n);   // "FreeType 2.13.3" or "stb_truetype"
// Device pixels per logical pixel, from the window's logical and pixel widths.
// This is the density the glyph atlas is baked at, so it is worth measuring
// before the first font is applied rather than after.
float mc_framebuffer_scale(int logical_w, int pixel_w);

struct MCConfig;
void app_panes_save_state(AppPanes* app, MCConfig* cfg);          // tabs/dirs -> config
void app_panes_apply_state(AppPanes* app, const MCConfig* cfg);   // config -> tabs/dirs

#endif // PANES_H
