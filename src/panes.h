#ifndef PANES_H
#define PANES_H

#include <stdbool.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "filesystem.h"
#include "img.h"      // ImgFormat, for what a quick view preview turned out to be
#include "remote.h"   // NetSite, for the site list AppPanes owns
#include "xfer.h"     // XferJob, what a background transfer reports through

static const int MAX_TABS = 8;
static const int MAX_DIR_HISTORY = 32;
static const int MAX_SEL_MEMORY = 64;

// Console limits. The backlog is the feature, so it is bounded where the data is
// appended rather than where it is drawn: a session of `ls` and one `cat` of a
// large file must not grow without limit, and the config file that carries the
// backlog across a restart has to stay a file a person can read.
static const int   MC_CONSOLE_MAX_ENTRIES = 200;   // transcript, oldest dropped
static const int   MC_CONSOLE_MAX_HIST    = 200;   // commands for Up/Down recall
static const size_t MC_CONSOLE_MAX_OUTPUT = 256 * 1024;  // one command's output
static const int   MC_CONSOLE_SAVE_ENTRIES = 50;   // written to the config
static const size_t MC_CONSOLE_SAVE_OUTPUT = 2048;  // per saved entry

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
    // The listing this pane is waiting for from the background worker, 0 when it
    // is not waiting, and when it asked (SDL ticks) so the status line can say
    // how long the server has been quiet.
    uint64_t remote_req = 0;
    uint64_t remote_req_ms = 0;

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

    // What to do with a target that already exists. Decided once, by the confirm
    // dialog, and honoured by every runner the plan can be handed to - the local
    // copy, the paste, the network worker, the extractor - because a choice that
    // only some of them understand is a choice that silently changes shape.
    // OW_ASK is "nobody has decided yet": what a plan looks like before the confirm
    // runs, and what one that found nothing existing keeps.
    enum Overwrite {
        OW_ASK = 0,
        OW_OVERWRITE,   // write over it, the way F5 has always done
        OW_SKIP,        // leave the existing target alone and count it
        OW_RENAME       // write beside it: "report.pdf" -> "report (2).pdf"
    };
    Overwrite overwrite = OW_ASK;
};

// .sfv verification. Hashing a 4 GB file is not a frame's work, so it runs on a
// thread with the rule BgTransfer and QvFetch already follow: the UI thread owns the
// record, the worker writes through atomics and a mutex, and the join happens before
// the record goes away.
enum { SFV_OK, SFV_MISMATCH, SFV_MISSING };

struct SfvRow {
    std::string name;
    unsigned long want = 0, got = 0;
    int status = SFV_OK;
};

struct SfvJob {
    std::atomic<int> state{0};              // 0 running, 1 done, 2 failed
    std::atomic<bool> cancel{false};
    std::atomic<bool> dismiss{false};       // the report window's Close button
    std::atomic<long long> done{0}, total{0};   // files checked of files listed
    std::string sfv_path, dir, err;         // fixed before the worker starts
    std::mutex mu;                          // guards rows and malformed
    std::vector<SfvRow> rows;
    int malformed = 0;                      // lines that were not a name-and-checksum pair
    bool settled = false;                   // the finish has been summarised once
    bool report_open = false;               // set when the job ends with something to show
    // The transfer this verification belongs to, kept so "re-fetch the bad ones"
    // has a source to fetch from. A verification started from the menu has none, and
    // reports without offering to fix anything.
    OpPlan plan;
    NetSite site;
    bool has_plan = false;
    std::thread worker;
};

// A copy or move across a network that runs on its own thread instead of
// blocking the panes. The counters are written by the worker and read by the
// frame that draws the transfer list, so they are atomic. The record itself
// belongs to the UI thread, which creates it, starts the worker, draws it and
// joins the worker before erasing the record - a std::thread that is still
// joinable when it is destroyed ends the process. `error` and `ok_names` are the
// worker's: it writes them and then stores the finishing state, and a frame that
// reads that state is allowed to read them.
//
// The counters - abort, done, total, files_done, files_total - are the XferJob's:
// the row's thread plans the transfer into files and hands them to the site's
// queue (xfer.h), and whichever queue thread moves a file reports into them.
struct BgTransfer : XferJob {
    enum State { BG_RUNNING = 0, BG_DONE, BG_FAILED, BG_ABORTED };

    std::atomic<int>       state;       // one of State
    std::atomic<bool>      settled;     // the finish has been handled once already
    std::atomic<bool>      dismiss;     // the row's button: retire it next frame

    // Fixed before the worker starts; the worker only reads them.
    bool   is_move;
    bool   download;        // true: server -> disk, false: disk -> server
    bool   resume = false;  // true: continue what the disk or the server already holds
    double t0;              // SDL_GetTicks() seconds at the start
    std::string src, dst;

    // What the transfer was actually doing, kept so that a stopped row is more than
    // a caption and a bar. Without the plan there is nothing to start again, and
    // without the login there is nothing to start it against. The site is a copy
    // rather than a pointer into the saved list for the same reason the worker takes
    // one by value: the user can edit or delete that site while the row sits there.
    OpPlan  plan;
    NetSite site;
    bool    resumable = false;   // a row that can be started again at all

    // The worker's answer. Read once state is no longer BG_RUNNING.
    std::string error;
    int ok_names = 0;
    // Names the plan left alone because the target was already there and the user
    // said skip. Written with `ok_names`, before the state that publishes them.
    int skipped_names = 0;
    // SDL_GetTicks() at the moment the worker stopped, written before the state
    // that publishes it. A finished row stays on screen long after the thread is
    // gone, so its "in Ns" is this minus t0 and not the clock minus t0.
    std::atomic<long long> t_end_ms{0};

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
    // A picture instead of text. Read after `done`, like `text`, so the same rule
    // covers both: the worker writes them, then publishes done, and only a frame
    // that saw done reads them.
    bool                is_image = false;
    int                 img_w = 0, img_h = 0;      // as decoded
    int                 img_src_w = 0, img_src_h = 0;   // as the file declares them
    ImgFormat           img_fmt = IMG_NONE;
    int                 img_orient = 0;            // the EXIF tag that was applied
    long long           img_bytes = 0;             // the file's own size
    std::vector<unsigned char> img_rgba;
    std::thread worker;
};

// A picture a quick view can draw. The pixels are kept because the window is
// resized with the mouse and a picture has to refit every frame, and the texture is
// made lazily on the first frame that actually draws it: a preview that is asked for
// and then arrowed past should not have cost a GL texture.
struct QvImage {
    int w = 0, h = 0;                       // the pixels held here
    int src_w = 0, src_h = 0;               // what the file declared
    ImgFormat fmt = IMG_NONE;
    int orientation = 0;                    // the EXIF tag that was applied
    long long file_bytes = 0;
    std::vector<unsigned char> rgba;
    unsigned long long tex = 0;             // 0 until it is first drawn
};

// One line of the console's backlog: what was typed, the directory it ran in,
// what it printed, and how it ended. `cwd` is stored per entry rather than once
// for the window because the whole point of a backlog that outlives navigation is
// that the commands in it ran in different places.
struct ConsoleEntry {
    std::string cwd;        // directory the command ran in
    std::string cmd;        // the line as typed
    std::string out;        // stdout+stderr, in reading order
    int  code = -1;         // exit code; -1 for a builtin, which has none
    bool builtin = false;   // cd / pwd / clear / history: no child process
    bool cancelled = false; // ended by Esc rather than by finishing
};

// A console command that is running. The worker appends to `out` under `mu` as
// the tool produces it, which is what lets a long `find` show its results while
// it is still running; the frame reads the same mutex. `cancel` is the UI's
// request and the worker's only reason to stop early. Same ownership rule as
// BgTransfer and QvFetch: the UI thread joins the worker, only once `done` is
// set, and always before the record goes away - a std::thread that is still
// joinable when it is destroyed ends the process.
struct ConsoleJob {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::mutex mu;          // guards `out` and `code`
    std::string out;
    int code = -1;
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
    CONF_OVERWRITE_EXTRACT, // copy (F5) out of an archive onto existing files
    CONF_SFV_VERIFY,   // a download landed in a folder that holds a .sfv
    CONF_EDIT_BINARY   // F4 on a file whose bytes are not text: edit it anyway?
};

// The answer a confirm's button or key recorded. It is consumed between frames
// rather than where it was pressed, because the answer starts an operation that
// pumps its own progress frames. The overwrite confirms answer with three of
// these - Overwrite, Skip, Rename - and Esc cancels the whole operation instead
// of answering the question.
enum ConfirmAnswer {
    CONF_ANS_NONE = 0,
    CONF_ANS_YES,        // the delete confirm's Yes
    CONF_ANS_NO,         // the delete confirm's No
    CONF_ANS_OVERWRITE,
    CONF_ANS_SKIP,
    CONF_ANS_RENAME,
    CONF_ANS_CANCEL,     // Esc on an overwrite confirm: nothing runs
    CONF_ANS_VERIFY,     // the .sfv offer's Verify
    CONF_ANS_NOT_NOW,    // its Not now: the files stay exactly as they are
    CONF_ANS_ALWAYS      // its Always: for this session, without writing the config
};

// A question Y/N cannot hold. A transfer that stopped leaves bytes on disk, and
// continuing them, throwing them away, and leaving the row alone are three
// different intentions, not two. The answers are positional: `choice_pick` is an
// index into the answers the dialog drew.
enum ConfirmChoice {
    CHOICE_NONE = 0,
    CHOICE_RESUME      // Resume / Start over / Skip
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
    // Which control in the operation dialog the keyboard is on: the field, or one
    // of its buttons. Tab walks it and Enter runs whatever it sits on. The ids are
    // fixed rather than positions in a list, because the dialog draws "Run in
    // background" only for a copy or move that crosses a network - the ring changes
    // length, and an id that meant Cancel keeps meaning that whatever else moved.
    int op_focus;
    char op_error[512];   // the dialog's own error line, cleared on open
    char status_msg[512]; // last operation result, shown in the status bar

    // Console (Ctrl+Enter). There is no command line: the console is a window
    // over the panes, and its backlog - every command and its output, oldest
    // first - outlives the window, the directory it was opened in, and the
    // process. `console_open` is the window; `console_log` is the backlog, and
    // hiding one never touches the other.
    bool console_open;
    // The console owns the keyboard only while its field holds it. Opened by
    // Ctrl+Enter it does; a click onto a pane row hands the keyboard back to the
    // panes and the window stays on screen as a transcript you can still read.
    bool console_field_active;   // the field is the active item this frame
    bool console_focus_request;  // one-shot: focus the field next frame
    char console_input[512];     // the line being typed
    // One-shot: the line was written from outside the text field, so the field has
    // to reload its own copy of it. See console_set_input().
    bool console_input_reload;
    std::vector<ConsoleEntry> console_log;      // the backlog, oldest first
    std::vector<std::string> console_hist;      // commands only, for Up/Down
    int console_hist_pos;        // == size() means "editing a fresh line"
    std::unique_ptr<ConsoleJob> console_job;    // non-null: a command is running
    // The user scrolled up inside the transcript: stop following the tail of a
    // running command, or the line being typed would be pushed off screen.
    bool console_scroll_lock;
    float console_w = 760.0f;    // remembered size, as the quick view's is
    float console_h = 420.0f;

    // Confirmation dialog
    AppConfirm confirm;
    char confirm_text[512];

    // The three-answer dialog. `choice_pick` is set by a button or a key and
    // consumed between frames, exactly the way a confirm's Yes is, because the
    // answer starts a thread and a thread is not started inside a draw call.
    ConfirmChoice choice = CHOICE_NONE;
    char choice_text[512] = "";
    BgTransfer* choice_transfer = nullptr;
    int choice_pick = -1;

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
    // A connect whose first listing has not come back yet. The listing runs on a
    // worker, so whether the connect worked is only known a few frames later:
    // then the dialogs close, or the pane goes back to `remote_connect_back`.
    uint64_t remote_connect_req = 0;
    char remote_connect_back[4096] = "";
    std::string remote_connect_target;
    // The connection log window, and the lines it has copied out of remote.cpp's
    // log so far (it asks only for newer ones each frame).
    bool netlog_open = false;
    bool netlog_follow = true;          // keep scrolled to the newest line
    uint64_t netlog_seq = 0;
    std::vector<NetLogLine> netlog;
    // Each line as drawn: a server reply in CP437 (an .nfo-style banner) is
    // transcoded once, when it arrives, not every frame. `netlog_page` is the
    // preview_codepage that was in force, so changing it redraws the lot.
    std::vector<std::string> netlog_shown;
    int netlog_page = -1;
    bool dirhist_open;    // Alt+Down: visited directories
    bool console_hist_open; // Alt+F8: the console's command history
    bool volumes_open;    // Alt+F1/F2: mounted drives
    int  volumes_target;  // pane index the volumes list navigates
    int  volumes_cursor = -1;  // the drive the arrow keys are on; -1 = not placed yet
    bool volumes_moved = false; // the cursor moved by key: scroll its row into view

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
    // The permission and ownership half of the same window. `props_mode`, `props_uid`
    // and `props_gid` are what the target has; the edit fields are what the user typed
    // and Apply commits. An empty owner or group field means "leave it alone": a
    // window that shows the current values has to be able to change one of three
    // things without the other two being re-entered.
    mode_t props_mode = 0;
    unsigned long props_uid = 0, props_gid = 0;
    char props_owner[96] = "";      // "name (uid)", for reading
    char props_group[96] = "";
    char props_edit_owner[96] = ""; // a name or a number, empty = unchanged
    char props_edit_group[96] = "";
    char props_edit_mode[16] = "";  // four octal digits, kept in step with the boxes
    mode_t props_edit_bits = 0;     // the boxes' state, and what Apply uses
    bool props_recursive = false;   // apply below this entry too
    bool props_remote = false;      // a site's row: SITE CHMOD only, no owner to set
    bool props_field_active = false;
    char props_remote_root[512] = "";
    char props_remote_path[4096] = "";
    char props_note[256] = "";      // what Apply reported, good or bad

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
    // Which progress-dialog button the keyboard is on: PROG_FOCUS_* in panes.cpp.
    // It starts on "Run in background" and never on Abort, so a stray Enter
    // cannot stop a transfer.
    int       prog_focus = -1;

    // Copy/move transfers running on their own thread instead of blocking the
    // panes, oldest first. The frame draws them; bg_reap() settles the finished
    // ones (join, refresh, status line) and retires the ones the user dismissed.
    // app_panes_free() asks the running ones to stop and joins them before the
    // app goes away.
    std::vector<std::unique_ptr<BgTransfer>> transfers;
    // The queue line over those rows. A batch is the rows started since the queue
    // was last idle, so the bar runs from empty to full once per burst of work
    // instead of jumping back each time a row finishes. 0 = no batch running.
    double    queue_t0 = 0.0;
    // The speed of each queue thread over the last few seconds, fed every frame.
    XferMeter queue_meter;
    // Quick view. `qv_cache` is the text already read, keyed by the path the pane
    // shows, so moving the cursor back onto a name costs nothing; `qv_fetches`
    // are the previews that are not on this disk and are still on their way.
    std::vector<std::pair<std::string, std::string>> qv_cache;
    std::vector<std::unique_ptr<QvFetch>> qv_fetches;
    // Pictures the quick view has decoded, oldest first, keyed by the same path the
    // text cache uses. Kept apart from `qv_cache` because the two hold things three
    // orders of magnitude apart in size, and one cap cannot serve both.
    std::vector<std::pair<std::string, std::unique_ptr<QvImage>>> qv_images;
    // 1 (default): a picture row is drawn as a picture. 0: it gets the hex dump it
    // always got. A line in the config file rather than a dialog setting, like
    // console_save.
    int qv_images_on = 1;
    // .sfv verification runs, oldest first, drawn as a row in the same strip while
    // they hash and as a report window once they finish.
    std::vector<std::unique_ptr<SfvJob>> sfv_jobs;
    // The size the F3 window was last at, in pixels. main.cpp switches off ImGui's
    // own .ini, so a floating window keeps its shape across sessions only if the
    // config carries it. The position is deliberately not saved: a window left at
    // coordinates from a monitor that has gone is one that cannot be grabbed.
    // Keep the default in sync with quickview_w/quickview_h in config.cpp.
    float qv_w = 720.0f;
    float qv_h = 460.0f;
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
    // Which of the answers a confirm recorded: the delete confirm's Yes or No, or
    // an overwrite confirm's Overwrite, Skip, Rename and the Esc that cancels the
    // whole operation. Consumed between frames, where an operation may start.
    int pending_confirm_answer = CONF_ANS_NONE;
    // "Always" in the verify-after-download offer. It changes behaviour for this
    // session and does not write the config file, so a user who clicked it once is
    // not stuck with it next time the app starts.
    bool sfv_always_session = false;
    // "ask" | "always" | "never", read from the config file at startup. The app
    // holds its own copy rather than reaching for the config struct, the same way it
    // holds its own theme and font: the panes module has no pointer to the config.
    char sfv_verify[16] = "ask";

    // Which alphabet the quick view reads a preview in: "auto" | "utf8" | "cp437".
    // Copied from the config for the same reason sfv_verify is: the preview code has
    // no config pointer, and the answer must be settled before a worker is started.
    char preview_codepage[16] = "auto";

    // The terminal F4 launches the editor in, straight from `terminal =` in the config.
    // Empty means the built-in list. Copied for the same reason as preview_codepage:
    // the launch code has no config pointer.
    char term_pref[128] = "";

    // Set when the binary-edit question is opened, so answering Yes launches the
    // editor for the file the question was about rather than whatever the cursor
    // moved to while it was open.
    char pending_edit_path[4100] = "";
    // The .sfv the verify offer is asking about, and the transfer it came from. The
    // transfer is kept because "re-fetch the bad ones" needs the plan that fetched
    // them, and a verification started from the menu has no such thing.
    std::string sfv_pending_path;
    std::string sfv_pending_dir;
    BgTransfer* sfv_pending_transfer = nullptr;
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
    // whoever asks first has to say so: a field that wants the keyboard on every
    // frame asks for it. Transient, never saved.
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
    bool show_icons = true;  // the small icon in front of each name (Options menu)
    // The launch bar under the menu, Total Commander's button bar: one button per
    // program, folder or file, kept in <settings>/launchbar. A click starts it.
    bool show_launchbar = true;
    struct LaunchItem {
        std::string path;      // '/'-separated, as the panes write paths
        bool is_dir = false;   // looked up once, when the button is added or loaded
        bool is_exec = false;
    };
    std::vector<LaunchItem> launch_items;
    float launch_bar_min_y = 0, launch_bar_max_y = -1;  // where it was last frame, for drops
    bool  os_drag_active = false;   // something is being dragged in from Explorer
    float os_drag_x = 0, os_drag_y = 0;
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
// Files dragged in from the desktop or Explorer. x/y are window coordinates, the
// ones ImGui draws in. A drop on the launch bar adds a button for each file.
void app_panes_drag_over(AppPanes* app, bool active, float x, float y);
void app_panes_drop_file(AppPanes* app, const char* path, float x, float y);
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
