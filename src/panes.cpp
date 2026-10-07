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
#include <cerrno>
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
    left.quick_view = false;
    right.quick_view = false;
    left.page_size = PAGE_SIZE;
    right.page_size = PAGE_SIZE;
    left_focus = true;
    active_path_pane = -1;
    op = OP_NONE;
    op_name[0] = '\0';
    op_input_active = false;
    status_msg[0] = '\0';
}

static int marked_count(const Pane& p) {
    int n = 0;
    for (size_t i = 0; i < p.marked.size(); i++)
        if (p.marked[i]) n++;
    return n;
}

static void toggle_mark(Pane& p, int idx) {
    if (idx < 0 || idx >= p.entry_count || idx >= (int)p.marked.size()) return;
    p.marked[idx] = !p.marked[idx];
}

void refresh_pane(Pane* pane, const char* path) {
    free_entries(pane->entries, pane->entry_count);
    strncpy(pane->current_dir, path, sizeof(pane->current_dir) - 1);
    pane->current_dir[sizeof(pane->current_dir) - 1] = '\0';
    pane->selected = 0;
    list_directory(path, &pane->entries, &pane->entry_count);
    pane->marked.assign(pane->entry_count, false);
}

void app_panes_refresh_left(AppPanes* app) {
    refresh_pane(&app->left, app->left.current_dir);
}

void app_panes_refresh_right(AppPanes* app) {
    refresh_pane(&app->right, app->right.current_dir);
}

// File list for one pane. Mouse hover must not create a second "selected" look;
// the selection bar only shows in the focused pane.
static void render_file_list(AppPanes* app, Pane& pane, bool focused, float width, const char* id) {
    ImGui::BeginChild(id, ImVec2(width, 0));
    // Measure how many rows actually fit, so Page Up/Down matches the visible page
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    pane.page_size = row_h > 0 ? (int)(ImGui::GetWindowSize().y / row_h) : PAGE_SIZE;
    if (pane.page_size < 1) pane.page_size = PAGE_SIZE;
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
        app->left_focus = focused;
    }
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    for (int i = 0; i < pane.entry_count; i++) {
        FileEntry* entry = pane.entries[i];
        bool selected = focused && (i == pane.selected);
        bool is_marked = i < (int)pane.marked.size() && pane.marked[i];

        // MC-style: marked entries get a green bar (in either pane)
        if (is_marked) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.55f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        if (is_directory(entry->name, entry->mode)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.6f, 1.0f, 1.0f));
        } else if (is_executable(entry->name, entry->mode)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
        }

        if (ImGui::Selectable(entry->name, selected)) {
            pane.selected = i;   // clicking the other pane still moves its hidden cursor
            app->left_focus = focused; // ...and focuses that pane
        }

        // Scroll the keyboard selection into view (child window doesn't do it itself)
        if (selected && pane.scroll_request) {
            ImGui::SetScrollY(ImGui::GetCursorPosY() - ImGui::GetWindowSize().y * 0.5f);
            pane.scroll_request = false;
        }

        if (is_directory(entry->name, entry->mode) || is_executable(entry->name, entry->mode)) {
            ImGui::PopStyleColor();
        }
        if (is_marked) ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleColor(); // HeaderHovered
    ImGui::EndChild();
}

// Quick view: file info + text preview for the current selection.
static void render_quick_view(Pane& pane, float width, float height, const char* id) {
    ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_Borders);

    if (pane.selected >= pane.entry_count) {
        ImGui::TextUnformatted("(empty)");
        ImGui::EndChild();
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
    ImGui::Text("Modified: %s", ctime(&e->mtime));
    ImGui::Separator();

    if (!is_directory(e->name, e->mode)) {
        char full[4100];
        snprintf(full, sizeof(full), "%s/%s", pane.current_dir, e->name);
        static char preview[4096];
        long n = fs_read_preview(full, preview, sizeof(preview) - 1);
        if (n < 0) {
            ImGui::TextUnformatted("(binary or unreadable)");
        } else {
            ImGui::TextUnformatted(preview);
        }
    }
    ImGui::EndChild();
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

    // Left pane body: file list, plus quick view on the right half when active
    float left_w = ImGui::GetContentRegionAvail().x;
    if (left.quick_view) {
        float half = left_w * 0.5f;
        render_file_list(app, left, left_focus, half, "left_pane");
        ImGui::SameLine();
        render_quick_view(left, half, child_h, "left_quickview");
    } else {
        render_file_list(app, left, left_focus, left_w, "left_pane");
    }

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

    // Right pane body: file list, plus quick view on the right half when active
    float right_w = ImGui::GetContentRegionAvail().x;
    if (right.quick_view) {
        float half = right_w * 0.5f;
        render_file_list(app, right, !left_focus, half, "right_pane");
        ImGui::SameLine();
        render_quick_view(right, half, child_h, "right_quickview");
    } else {
        render_file_list(app, right, !left_focus, right_w, "right_pane");
    }
    ImGui::EndTable();

    // Bottom bar: op bar when an op is pending, otherwise the status line.
    // Single line, fixed height = reserved space.
    ImGui::Separator();
    if (app->op != OP_NONE) {
        const char* op_label = (app->op == OP_RENAME) ? "Rename" : "New dir";
        ImGui::TextUnformatted(op_label);
        ImGui::SameLine();
        ImGui::SetKeyboardFocusHere(); // grab keyboard when the op bar appears
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##opname", app->op_name, sizeof(app->op_name));
        app->op_input_active = ImGui::IsItemActive();
    } else {
        app->op_input_active = false;
        char status[4100];
        Pane& active = left_focus ? left : right;
        int marks = marked_count(active);
        if (app->status_msg[0]) {
            snprintf(status, sizeof(status), "%s", app->status_msg);
        } else if (active.selected < active.entry_count) {
            FileEntry* e = active.entries[active.selected];
            char mode[16];
            format_mode(e->mode, mode);
            if (marks > 0) {
                snprintf(status, sizeof(status), "%s  %s  %lld  marked: %d",
                         e->name, mode, (long long)e->size, marks);
            } else {
                snprintf(status, sizeof(status), "%s  %s  %lld", e->name, mode, (long long)e->size);
            }
        } else {
            snprintf(status, sizeof(status), "Ready - %s pane, %d entries", left_focus ? "Left" : "Right", active.entry_count);
        }
        ImGui::InputText("command", status, sizeof(status));
    }

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

// Run fs op over marked entries, or just the current selection if none marked.
// `act` performs the op on one src path; returns count of successes.
static int run_on_targets(AppPanes* app, Pane& pane, bool (*act)(const char*, const char*), const char* dst_dir, const char* name_override) {
    int ok = 0;
    int marks = marked_count(pane);
    for (int i = 0; i < pane.entry_count; i++) {
        if (marks == 0 && i != pane.selected) continue;
        if (marks > 0 && !pane.marked[i]) continue;
        FileEntry* e = pane.entries[i];
        if (strcmp(e->name, "..") == 0) continue;
        char src[4100];
        snprintf(src, sizeof(src), "%s/%s", pane.current_dir, e->name);
        const char* nm = name_override ? name_override : e->name;
        char dst[4100];
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, nm);
        if (act(src, dst)) ok++;
    }
    return ok;
}

void app_panes_commit_op(AppPanes* app) {
    Pane& pane = app->left_focus ? app->left : app->right;
    if (app->op == OP_RENAME) {
        if (pane.selected < pane.entry_count && strcmp(pane.entries[pane.selected]->name, "..") != 0) {
            FileEntry* e = pane.entries[pane.selected];
            char src[4100], dst[4100];
            snprintf(src, sizeof(src), "%s/%s", pane.current_dir, e->name);
            snprintf(dst, sizeof(dst), "%s/%s", pane.current_dir, app->op_name);
            if (rename(src, dst) == 0) {
                snprintf(app->status_msg, sizeof(app->status_msg), "Renamed %s -> %s", e->name, app->op_name);
            } else {
                snprintf(app->status_msg, sizeof(app->status_msg), "Rename failed: %s", strerror(errno));
            }
            refresh_pane(&pane, pane.current_dir);
        }
    } else if (app->op == OP_MKDIR) {
        char dst[4100];
        snprintf(dst, sizeof(dst), "%s/%s", pane.current_dir, app->op_name);
        if (fs_make_dir(dst)) {
            snprintf(app->status_msg, sizeof(app->status_msg), "Created dir %s", app->op_name);
            refresh_pane(&pane, pane.current_dir);
        } else {
            snprintf(app->status_msg, sizeof(app->status_msg), "mkdir failed: %s", strerror(errno));
        }
    }
    app->op = OP_NONE;
    app->op_name[0] = '\0';
    app->op_input_active = false;
}

void app_panes_cancel_op(AppPanes* app) {
    app->op = OP_NONE;
    app->op_name[0] = '\0';
    app->op_input_active = false;
}

void app_panes_handle_key(AppPanes* app, SDL_Keycode key, SDL_Keymod mod) {
    // Handle keyboard input
    Pane& active = app->left_focus ? app->left : app->right;
    Pane& other = app->left_focus ? app->right : app->left;

    // Ctrl+Space: toggle mark on the current entry (MC-style multi-select)
    if (key == SDLK_SPACE && (mod & SDL_KMOD_CTRL)) {
        toggle_mark(active, active.selected);
        return;
    }

    // Ctrl+T: toggle quick view for the focused pane
    if (key == SDLK_T && (mod & SDL_KMOD_CTRL)) {
        active.quick_view = !active.quick_view;
        return;
    }

    // F2: rename (op bar with name prefilled)
    if (key == SDLK_F2) {
        if (active.selected < active.entry_count && strcmp(active.entries[active.selected]->name, "..") != 0) {
            app->op = OP_RENAME;
            snprintf(app->op_name, sizeof(app->op_name), "%s", active.entries[active.selected]->name);
        }
        return;
    }

    // F7: mkdir (op bar with empty name)
    if (key == SDLK_F7) {
        app->op = OP_MKDIR;
        app->op_name[0] = '\0';
        return;
    }

    // F4: delete marked entries (or current selection)
    if (key == SDLK_F4) {
        int ok = 0;
        int marks = marked_count(active);
        for (int i = 0; i < active.entry_count; i++) {
            if (marks == 0 && i != active.selected) continue;
            if (marks > 0 && !active.marked[i]) continue;
            FileEntry* e = active.entries[i];
            if (strcmp(e->name, "..") == 0) continue;
            char src[4100];
            snprintf(src, sizeof(src), "%s/%s", active.current_dir, e->name);
            if (fs_delete_entry(src)) ok++;
        }
        snprintf(app->status_msg, sizeof(app->status_msg), ok ? "Deleted %d entries" : "Delete failed: %s", ok, strerror(errno));
        refresh_pane(&active, active.current_dir);
        return;
    }

    // F5: copy marked entries (or current selection) to the other pane
    if (key == SDLK_F5) {
        int ok = run_on_targets(app, active, fs_copy_entry, other.current_dir, nullptr);
        snprintf(app->status_msg, sizeof(app->status_msg), ok ? "Copied %d entries to %s" : "Copy failed: %s", ok, other.current_dir, strerror(errno));
        refresh_pane(&other, other.current_dir);
        return;
    }

    // F6: move marked entries (or current selection) to the other pane
    if (key == SDLK_F6) {
        int ok = run_on_targets(app, active, fs_move_entry, other.current_dir, nullptr);
        snprintf(app->status_msg, sizeof(app->status_msg), ok ? "Moved %d entries to %s" : "Move failed: %s", ok, other.current_dir, strerror(errno));
        refresh_pane(&active, active.current_dir);
        refresh_pane(&other, other.current_dir);
        return;
    }

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
                int page = active.page_size > 0 ? active.page_size : PAGE_SIZE;
                active.selected = (active.selected >= page) ? active.selected - page : 0;
                active.scroll_request = true;
            }
            break;
        case SDLK_PAGEDOWN:
            if (active.selected < active.entry_count - 1) {
                int page = active.page_size > 0 ? active.page_size : PAGE_SIZE;
                active.selected = (active.selected + page < active.entry_count)
                                 ? active.selected + page
                                 : active.entry_count - 1;
                active.scroll_request = true;
            }
            break;
        case SDLK_END:
            // End: jump to the last entry
            if (active.entry_count > 0) {
                active.selected = active.entry_count - 1;
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
