#include "panes.h"
#include "filesystem.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <libgen.h>
#include <float.h>

// Rows per Page Up / Page Down step (approx. visible rows in a 800px pane)
static const int PAGE_SIZE = 18;

AppPanes::AppPanes() {
    left.entries = nullptr;
    left.entry_count = 0;
    left.selected = 0;
    left.scroll_request = false;
    right.entries = nullptr;
    right.entry_count = 0;
    right.selected = 0;
    right.scroll_request = false;
    left.path_edit_active = false;
    right.path_edit_active = false;
    left_focus = true;
    active_path_pane = -1;
}

void refresh_pane(Pane* pane, const char* path) {
    free_entries(pane->entries, pane->entry_count);
    strncpy(pane->current_dir, path, sizeof(pane->current_dir) - 1);
    pane->current_dir[sizeof(pane->current_dir) - 1] = '\0';
    pane->selected = 0;
    list_directory(path, &pane->entries, &pane->entry_count);
}

void app_panes_refresh_left(AppPanes* app) {
    refresh_pane(&app->left, app->left.current_dir);
}

void app_panes_refresh_right(AppPanes* app) {
    refresh_pane(&app->right, app->right.current_dir);
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
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Top menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Commands")) {
            if (ImGui::MenuItem("Change Directory", "F3")) {
                // TODO: drive selection dialog
            }
            if (ImGui::MenuItem("Refresh", "F5")) {
                left_focus ? app_panes_refresh_left(app) : app_panes_refresh_right(app);
            }
            if (ImGui::MenuItem("Quit", "Alt+Q")) {
                // handled in main loop
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                // TODO
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Dual pane layout: 2-column table guarantees a 50/50 split.
    // Explicitly reserve room for the bottom command bar so the whole UI
    // fits inside the window (no overflow, no scrollbar).
    // Vertical budget: title bar + separator + pane body, then command bar.
    // The table auto-sizes to its content, so we give each pane's child an
    // explicit height so the total exactly fills the window.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float frame = ImGui::GetFrameHeightWithSpacing();
    const float bar_h = frame + 4.0f;   // separator + single-line command input
    const float title_h = frame;        // pane title bar
    const float sep_h = 6.0f;           // separator under the title
    float child_h = avail.y - bar_h - title_h - sep_h;
    ImGui::BeginTable("panes_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInner);
    ImGui::TableNextColumn();

    // Left pane
    // Editable path bar, fixed while the list scrolls.
    // Click to type/paste a path, Enter to navigate, Escape to cancel.
    // Bright blue = focused, dim = unfocused.
    if (!left.path_edit_active)
        snprintf(left.path_edit, sizeof(left.path_edit), "%s", left.current_dir);
    ImVec4 bar_bg   = left_focus ? ImVec4(0.2f, 0.45f, 0.85f, 1.0f) : ImVec4(0.15f, 0.17f, 0.2f, 1.0f);
    ImVec4 bar_text = left_focus ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)  : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, bar_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, bar_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, bar_bg);
    ImGui::PushStyleColor(ImGuiCol_Text, bar_text);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(1.0f, 1.0f, 1.0f, 0.3f));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##leftpath", left.path_edit, sizeof(left.path_edit));
    left.path_edit_active = ImGui::IsItemActive();
    if (left.path_edit_active)
        left_focus = true; // editing a pane's path focuses that pane
    if (left.path_edit_active)
        app->active_path_pane = 0;
    else if (app->active_path_pane == 0)
        app->active_path_pane = -1;
    ImGui::PopStyleColor(5);
    ImGui::Separator();

    // Left pane file listing (selection bar only in the focused pane;
    // mouse hover must not create a second "selected" look)
    ImGui::BeginChild("left_pane", ImVec2(0, child_h));
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
        left_focus = true;
    }
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    for (int i = 0; i < left.entry_count; i++) {
        FileEntry* entry = left.entries[i];
        bool selected = left_focus && (i == left.selected);
        
        if (is_directory(entry->name, entry->mode)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
        } else if (is_executable(entry->name, entry->mode)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
        }
        
        if (ImGui::Selectable(entry->name, selected)) {
            left.selected = i;   // clicking the other pane still moves its hidden cursor
            left_focus = true;   // ...and focuses that pane
        }
        
        // Scroll the keyboard selection into view (child window doesn't do it itself)
        if (selected && left.scroll_request) {
            ImGui::SetScrollY(ImGui::GetCursorPosY() - ImGui::GetWindowSize().y * 0.5f);
            left.scroll_request = false;
        }

        if (is_directory(entry->name, entry->mode) || is_executable(entry->name, entry->mode)) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::PopStyleColor(); // HeaderHovered
    ImGui::EndChild();

    ImGui::TableNextColumn();

    // Right pane
    // Editable path bar, fixed while the list scrolls.
    // Click to type/paste a path, Enter to navigate, Escape to cancel.
    // Bright blue = focused, dim = unfocused.
    if (!right.path_edit_active)
        snprintf(right.path_edit, sizeof(right.path_edit), "%s", right.current_dir);
    ImVec4 r_bar_bg   = !left_focus ? ImVec4(0.2f, 0.45f, 0.85f, 1.0f) : ImVec4(0.15f, 0.17f, 0.2f, 1.0f);
    ImVec4 r_bar_text = !left_focus ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)  : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, r_bar_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, r_bar_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, r_bar_bg);
    ImGui::PushStyleColor(ImGuiCol_Text, r_bar_text);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(1.0f, 1.0f, 1.0f, 0.3f));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##rightpath", right.path_edit, sizeof(right.path_edit));
    right.path_edit_active = ImGui::IsItemActive();
    if (right.path_edit_active)
        left_focus = false; // editing a pane's path focuses that pane
    if (right.path_edit_active)
        app->active_path_pane = 1;
    else if (app->active_path_pane == 1)
        app->active_path_pane = -1;
    ImGui::PopStyleColor(5);
    ImGui::Separator();

    // Right pane file listing (selection bar only in the focused pane;
    // mouse hover must not create a second "selected" look)
    ImGui::BeginChild("right_pane", ImVec2(0, child_h));
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
        left_focus = false;
    }
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    for (int i = 0; i < right.entry_count; i++) {
        FileEntry* entry = right.entries[i];
        bool selected = !left_focus && (i == right.selected);
        
        if (is_directory(entry->name, entry->mode)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
        } else if (is_executable(entry->name, entry->mode)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
        }
        
        if (ImGui::Selectable(entry->name, selected)) {
            right.selected = i;  // clicking the other pane still moves its hidden cursor
            left_focus = false;  // ...and focuses that pane
        }
        
        // Scroll the keyboard selection into view (child window doesn't do it itself)
        if (selected && right.scroll_request) {
            ImGui::SetScrollY(ImGui::GetCursorPosY() - ImGui::GetWindowSize().y * 0.5f);
            right.scroll_request = false;
        }

        if (is_directory(entry->name, entry->mode) || is_executable(entry->name, entry->mode)) {
            ImGui::PopStyleColor();
        }
    }
    ImGui::PopStyleColor(); // HeaderHovered
    ImGui::EndChild();
    ImGui::EndTable();

    // Bottom status/command bar (single line, fixed height = reserved space)
    ImGui::Separator();
    char status[4100];
    snprintf(status, sizeof(status), "Ready - %s pane selected, %d entries",
             left_focus ? "Left" : "Right",
             left_focus ? left.entry_count : right.entry_count);
    ImGui::InputText("command", status, sizeof(status));

    ImGui::End();
}

AppPanes* app_panes_init() {
    AppPanes* app = new AppPanes();
    
    // Initialize left pane
    refresh_pane(&app->left, getenv("HOME") ? getenv("HOME") : "/");
    
    // Initialize right pane
    refresh_pane(&app->right, getenv("HOME") ? getenv("HOME") : "/");
    
    return app;
}

void app_panes_commit_path(AppPanes* app) {
    // Navigate the pane whose path bar is being edited to the typed path.
    // Called from the event loop on Enter (direct SDL key, no frame lag).
    Pane* pane = (app->active_path_pane == 1) ? &app->right : &app->left;
    if (app->active_path_pane < 0) return;
    DIR* probe = opendir(pane->path_edit); // only navigate to real directories
    if (probe) {
        closedir(probe);
        refresh_pane(pane, pane->path_edit);
        snprintf(pane->path_edit, sizeof(pane->path_edit), "%s", pane->current_dir);
    }
    // Invalid path: keep the typed text so the user can fix it.
}

void app_panes_free(AppPanes* app) {
    free_entries(app->left.entries, app->left.entry_count);
    free_entries(app->right.entries, app->right.entry_count);
    delete app;
}

void app_panes_handle_key(AppPanes* app, SDL_Keycode key) {
    // Handle keyboard input
    Pane& active = app->left_focus ? app->left : app->right;
    
    switch (key) {
        case SDLK_UP:
            if (active.selected > 0) {
                active.selected--;
                active.scroll_request = true;
            }
            break;
        case SDLK_DOWN:
            if (active.selected < active.entry_count - 1) {
                active.selected++;
                active.scroll_request = true;
            }
            break;
        case SDLK_PAGEUP:
            if (active.selected > 0) {
                active.selected = (active.selected >= PAGE_SIZE) ? active.selected - PAGE_SIZE : 0;
                active.scroll_request = true;
            }
            break;
        case SDLK_PAGEDOWN:
            if (active.selected < active.entry_count - 1) {
                active.selected = (active.selected + PAGE_SIZE < active.entry_count)
                                 ? active.selected + PAGE_SIZE
                                 : active.entry_count - 1;
                active.scroll_request = true;
            }
            break;
        case SDLK_TAB:
            app->left_focus = !app->left_focus;
            break;
        case SDLK_RETURN:
            // Enter: open directory or execute file
            if (active.selected < active.entry_count) {
                FileEntry* entry = active.entries[active.selected];
                if (strcmp(entry->name, "..") == 0) {
                    // ".." — go to parent with a clean path
                    char parentbuf[4096];
                    strncpy(parentbuf, active.current_dir, sizeof(parentbuf) - 1);
                    parentbuf[sizeof(parentbuf) - 1] = '\0';
                    char* parent = dirname(parentbuf);
                    refresh_pane(&active, parent);
                } else if (is_directory(entry->name, entry->mode)) {
                    char path[4100];
                    snprintf(path, sizeof(path), "%s/%s", active.current_dir, entry->name);
                    refresh_pane(&active, path);
                } else if (is_executable(entry->name, entry->mode)) {
                    // Execute file (future: show output in viewer)
                }
            }
            break;
        case SDLK_BACKSPACE:
            // Backspace: go to parent directory
            {
                char* parent = dirname(active.current_dir);
                if (parent) {
                    refresh_pane(&active, parent);
                }
            }
            break;
        case SDLK_HOME:
            // Home: go to home directory
            {
                const char* home = getenv("HOME");
                if (home) {
                    refresh_pane(&active, home);
                }
            }
            break;
        case SDLK_R:
            // Refresh pane
            app->left_focus ? app_panes_refresh_left(app) : app_panes_refresh_right(app);
            break;
        default:
            break;
    }
}
