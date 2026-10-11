// Dear ImGui: standalone example application for SDL3 + OpenGL
#include "imgui.h"
#include "imgui_internal.h" // for ClearActiveID (not in the public header in this version)
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <cstdio>
#include <cstring>

// Our application code
#include "panes.h"
#include "config.h"
#include "theme.h"
#include "version.h"

// Headless identity output: no SDL video, no ImGui, nothing to lay out. It is
// the one path a packager checks from a terminal, so it is handled before SDL is
// initialized rather than after a window has failed to open on a headless box.
// The lines are formatted into buffers and written with fputs: this project keeps
// printf out of the C++ sources.
static int print_version(void) {
    char line[256];
    app_identity_line(line, sizeof(line));

    int v = SDL_GetVersion();
    char built[256];
    snprintf(built, sizeof(built), "Dear ImGui %s, SDL %d.%d.%d\n", IMGUI_VERSION,
             SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));

    char cfgp[1024];
    mc_config_path(cfgp, sizeof(cfgp));
    char settings[1200];
    snprintf(settings, sizeof(settings), "settings %s\n", cfgp);

    // Which rasterizer this build has is the first thing a packager wants to know:
    // it decides whether the hinting setting in the config means anything at all.
    char raster[64];
    mc_font_rasterizer(raster, sizeof(raster));
    char font_line[160];
    snprintf(font_line, sizeof(font_line), "text rasterized with %s%s\n", raster,
             mc_font_hinting_available() ? " (hinting selectable)" : " (no hinting available)");

    // The same question one layer up: does this build talk to servers at all, and
    // can it do it over TLS. A build without libcurl still runs; only the remote
    // panes are missing, and this is where that becomes visible.
    char ftp[96];
    net_what(ftp, sizeof(ftp));
    char net_line[200];
    snprintf(net_line, sizeof(net_line), "remote sites: %s%s\n", ftp,
             net_available() ? "" : " (install libcurl and rebuild to browse FTP)");

    fputs(line, stdout); fputc('\n', stdout);
    fputs(built, stdout);
    fputs(settings, stdout);
    fputs(font_line, stdout);
    fputs(net_line, stdout);
    return 0;
}

static int print_usage(const char* argv0) {
    const char* name = strrchr(argv0, '/');
    char line[1200];
    snprintf(line, sizeof(line),
             "usage: %s [-v|--version] [-h|--help]\n"
             "%s %s - a Total Commander style file manager (Dear ImGui + SDL3)\n",
             name ? name + 1 : MC_APP_BINARY, MC_APP_NAME, MC_VERSION);
    fputs(line, stdout);
    return 0;
}

// The window's framebuffer scale: device pixels per logical pixel. SDL's
// SDL_GetWindowDisplayScale() additionally folds in the desktop content scale,
// which is about how large the window should look rather than how many pixels a
// glyph gets, so it is not the number the font atlas wants.
static float window_pixel_scale(SDL_Window* window)
{
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window, &w, &h);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    return mc_framebuffer_scale(w, pw);
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) return print_version();
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))    return print_usage(argv[0]);
    }

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
    SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    // Create with the saved position baked in: setting the position after
    // creation gets overridden by the window manager when it maps the window.
    SDL_Window* window;
    {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, MC_APP_NAME);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, cfg.win_w);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, cfg.win_h);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
        // High-DPI: ask for a framebuffer in device pixels. Without this the window
        // is given a 1x framebuffer and the compositor stretches it over the real
        // one, so on a 2x display every glyph is a 1x bitmap blown up to twice its
        // size - the usual reason a whole app looks soft on a laptop screen.
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                              (cfg.win_x >= 0 && cfg.win_y >= 0) ? cfg.win_x : SDL_WINDOWPOS_CENTERED);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                              (cfg.win_x >= 0 && cfg.win_y >= 0) ? cfg.win_y : SDL_WINDOWPOS_CENTERED);
        window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
    }
    if (!window)
        window = SDL_CreateWindow(MC_APP_NAME, cfg.win_w, cfg.win_h, window_flags);
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

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Init our application panes
    AppPanes* panes = app_panes_init();
    app_panes_set_sdl_window(panes, window);
    app_panes_apply_state(panes, &cfg);   // restore last session's tabs/dirs

    // Style and font come from the panes' state, which apply_state filled from
    // the config: the Options dialog and the startup path read the same three
    // values, so a theme or font the dialog changed is already in effect here.
    //
    // The density is settled before the font is baked, not after. ImGui rasterizes
    // glyphs at io.DisplayFramebufferScale, and AddFontDefault() chooses between
    // its two built-in fonts by the size that will reach the screen - both decide
    // during the call below, and neither has seen a frame yet.
    float scale = window_pixel_scale(window);
    io.DisplayFramebufferScale = ImVec2(scale, scale);
    app_panes_set_display_scale(panes, scale);

    mc_apply_theme(panes->theme);
    app_font_apply(panes->font, panes->font_size, panes->hinting);
    panes->font_dirty = false;   // the scale that change asked for is already baked in

    // Main loop
    bool done = false;
    char last_title[512] = "";
    while (!done)
    {
        // Poll and handle events
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
                done = true;

            // The window moved to a monitor with another density, or the desktop
            // scaling changed under it: the glyphs on screen were rasterized for
            // the old one, so the atlas goes back on the list to be rebuilt.
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED ||
                event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                if (event.window.windowID == SDL_GetWindowID(window)) {
                    float s = window_pixel_scale(window);
                    io.DisplayFramebufferScale = ImVec2(s, s);
                    app_panes_set_display_scale(panes, s);
                }
            }

            if (event.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = event.key.key;
                SDL_Keymod m = event.key.mod;

                // Modal confirm dialog: every key goes to the pane handler (Y/N/Esc).
                if (panes->confirm != CONF_NONE) {
                    app_panes_handle_key(panes, k, m);
                    continue;
                }

                // An operation dialog is up (F5/F6/F7/F2/Shift+F4): Enter runs what
                // the keyboard ring is on, Tab moves the ring, Esc cancels. Everything
                // else (typing, caret keys) goes to ImGui, which owns the text field.
                if (panes->op != OP_NONE) {
                    if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE || k == SDLK_TAB)
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

        // A font the Options dialog picked is rebuilt here too: ImGui does not
        // let the atlas change while a frame is open.
        if (panes->font_dirty)
            app_panes_apply_font(panes);

        // The title bar follows the active pane. Compared first, because setting
        // it every frame is a round trip to the window manager for a string that
        // almost never changes.
        char title[512];
        app_window_title(panes, title, sizeof(title));
        if (strcmp(title, last_title)) {
            SDL_SetWindowTitle(window, title);
            snprintf(last_title, sizeof(last_title), "%s", title);
        }

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
