// Dear ImGui: standalone example application for SDL3 + OpenGL
#include "imgui.h"
#include "imgui_internal.h" // for ClearActiveID (not in the public header in this version)
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

// Our application code
#include "panes.h"

int main(int, char**)
{
    // Setup SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        printf("Error: SDL_Init(): %s\n", SDL_GetError());
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
    SDL_Window* window = SDL_CreateWindow("mc-clone", 1280, 800, window_flags);
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

    // Style: Dark theme like MC
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Init our application panes
    AppPanes* panes = app_panes_init();

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
                if (event.key.key == SDLK_TAB) {
                    // Tab always switches panes (MC-style). Keep it out of ImGui's
                    // hands so its focus-cycling can't grab the path text field,
                    // and end any active text edit (reverts on the next frame).
                    if (ImGui::GetIO().WantTextInput)
                        ImGui::ClearActiveID();
                    app_panes_handle_key(panes, event.key.key, event.key.mod);
                    continue;
                }
            }

            ImGui_ImplSDL3_ProcessEvent(&event);

            // Pass keyboard events to panes for navigation,
            // but not while an ImGui text field (e.g. the path bar) is being edited
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (ImGui::GetIO().WantTextInput) {
                    // A text field is active. Enter commits the focused field;
                    // Escape cancels a pending op.
                    if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                        if (panes->op_input_active)
                            app_panes_commit_op(panes);
                        else
                            app_panes_commit_path(panes);
                    } else if (event.key.key == SDLK_ESCAPE && panes->op_input_active) {
                        app_panes_cancel_op(panes);
                    }
                } else {
                    app_panes_handle_key(panes, event.key.key, event.key.mod);
                }
            }
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
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

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
