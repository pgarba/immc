// Dear ImGui: standalone example application for SDL3 + OpenGL
#include "imgui.h"
#include "imgui_internal.h" // for ClearActiveID (not in the public header in this version)
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

// Our application code
#include "panes.h"
#include "config.h"
#include "theme.h"

int main(int, char**)
{
    // Load user configuration (~/.config/mc-clone/config)
    MCConfig cfg;
    mc_config_load(&cfg);

    // Setup SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Select GL version
    const char* glsl_version = nullptr;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    // Create with the saved position baked in: setting the position after
    // creation gets overridden by the window manager when it maps the window.
    SDL_Window* window;
    {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "mc-clone");
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, cfg.win_w);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, cfg.win_h);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                              (cfg.win_x >= 0 && cfg.win_y >= 0) ? cfg.win_x : SDL_WINDOWPOS_CENTERED);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                              (cfg.win_x >= 0 && cfg.win_y >= 0) ? cfg.win_y : SDL_WINDOWPOS_CENTERED);
        window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
    }
    if (!window)
        window = SDL_CreateWindow("mc-clone", cfg.win_w, cfg.win_h, window_flags);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // Note: ImGui's own keyboard navigation (NavEnableKeyboard) is intentionally
    // disabled — arrow keys, Tab, Enter etc. are handled by app_panes_handle_key
    // so the panes behave like MC instead of ImGui widget focus.

    // Style: theme from config (dark | light | classic | gruvbox |
    // gruvbox-light | solarized-dark | solarized-light)
    mc_apply_theme(cfg.theme);

    // Fonts: custom TTF from config, or the built-in font scaled to font_size.
    ImFontAtlas* fonts = ImGui::GetIO().Fonts;
    if (cfg.font[0] != '\0') {
        float fsize = cfg.font_size > 0.0f ? cfg.font_size : 16.0f;
        if (!fonts->AddFontFromFileTTF(cfg.font, fsize))
            fonts->AddFontDefault();
    } else if (cfg.font_size > 0.0f) {
        ImGui::GetStyle().FontScaleMain = cfg.font_size / 13.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Init our application panes
    AppPanes* panes = app_panes_init();
    app_panes_set_sdl_window(panes, window);
    app_panes_apply_state(panes, &cfg);   // restore last session's tabs/dirs

    // Main loop
    bool done = false;
    while (!done)
    {
        // Poll and handle events
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
                done = true;

            if (event.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = event.key.key;
                SDL_Keymod m = event.key.mod;

                // Modal confirm dialog: every key goes to the pane handler (Y/N/Esc).
                if (panes->confirm != CONF_NONE) {
                    app_panes_handle_key(panes, k, m);
                    continue;
                }

                // An operation dialog is up (F5/F6/F7/F2/Shift+F4): Enter runs it,
                // Esc cancels, everything else (typing, caret keys) goes to ImGui.
                if (panes->op != OP_NONE) {
                    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE)
                        app_panes_handle_key(panes, k, m);
                    else
                        ImGui_ImplSDL3_ProcessEvent(&event);
                    continue;
                }

                // A pane's path bar is being edited.
                if (panes->active_path_pane >= 0) {
                    if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
                        app_panes_commit_path(panes);
                    else if (k == SDLK_ESCAPE) {
                        ImGui::ClearActiveID();
                        panes->active_path_pane = -1;
                    } else
                        ImGui_ImplSDL3_ProcessEvent(&event);
                    continue;
                }

                // Search window's pattern field has focus: typing goes to ImGui.
                if (panes->search_input_active) {
                    if (k == SDLK_ESCAPE) {
                        panes->search_open = false;
                        panes->search_input_active = false;
                        ImGui::ClearActiveID();
                    } else
                        ImGui_ImplSDL3_ProcessEvent(&event);
                    continue;
                }

                // Navigation / function / shortcut keys are the app's;
                // printable keys fall through to the command line InputText.
                if (app_key_is_special(k, m, panes)) {
                    app_panes_handle_key(panes, k, m);
                    continue;
                }
                ImGui_ImplSDL3_ProcessEvent(&event);
                continue;
            }

            ImGui_ImplSDL3_ProcessEvent(&event);
        }

        if (panes->quit_request)
            done = true;

        // Deferred actions from menus/dialog buttons: run outside the ImGui
        // frame so copy/move can pump their own progress UI.
        if (panes->pending_op || panes->pending_confirm_yes)
            app_panes_run_pending(panes);

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Render our application
        app_panes_render(panes);

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        SDL_GetWindowSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(mc_theme_colors()->window_bg.x,
                     mc_theme_colors()->window_bg.y,
                     mc_theme_colors()->window_bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

    // Save window geometry and pane tabs back to the config file
    SDL_GetWindowPosition(window, &cfg.win_x, &cfg.win_y);
    SDL_GetWindowSize(window, &cfg.win_w, &cfg.win_h);
    app_panes_save_state(panes, &cfg);
    mc_config_save(&cfg);

    // Cleanup
    app_panes_free(panes);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
