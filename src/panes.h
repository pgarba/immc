#ifndef PANES_H
#define PANES_H

#include <stdbool.h>
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
};

class AppPanes {
public:
    Pane left;
    Pane right;
    bool left_focus;
    int active_path_pane; // -1 = none, 0 = left, 1 = right (path InputText has focus)

    AppPanes();
    void render();
    void refresh_pane(Pane& pane);
};

// C-style interface functions
AppPanes* app_panes_init();
void app_panes_render(AppPanes* app);
void app_panes_free(AppPanes* app);
void app_panes_handle_key(AppPanes* app, SDL_Keycode key);
void app_panes_commit_path(AppPanes* app); // navigate to the typed path in the focused path bar

#endif // PANES_H
