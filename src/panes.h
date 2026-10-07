#ifndef PANES_H
#define PANES_H

#include <stdbool.h>
#include <vector>
#include <SDL3/SDL.h>
#include "filesystem.h"

struct Pane {
    char current_dir[4096];
    FileEntry** entries;
    int entry_count;
    int selected;
    bool scroll_request; // set when keyboard moves the selection; render scrolls to it
    char path_edit[4096]; // edit buffer for the path bar
    bool path_edit_active; // true while the path InputText has keyboard focus
    std::vector<bool> marked; // multi-select flags, parallel to entries
};

// Pending file operation shown in the bottom op bar
enum AppOp {
    OP_NONE = 0,
    OP_RENAME,   // op_name = new name for the target entry
    OP_MKDIR     // op_name = new directory name
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

    AppPanes();
    void render();
    void refresh_pane(Pane& pane);
};

// C-style interface functions
AppPanes* app_panes_init();
void app_panes_render(AppPanes* app);
void app_panes_free(AppPanes* app);
void app_panes_handle_key(AppPanes* app, SDL_Keycode key, SDL_Keymod mod);
void app_panes_commit_path(AppPanes* app); // navigate to the typed path in the focused path bar
void app_panes_commit_op(AppPanes* app);   // execute the pending op (rename/mkdir) from the op bar
void app_panes_cancel_op(AppPanes* app);   // cancel a pending op

#endif // PANES_H
