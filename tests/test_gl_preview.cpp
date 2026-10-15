// A picture, on a screen.
//
// The other suites prove the parts: test-img proves the decode, test-panes proves the
// read-and-cache pipeline, test-texture proves the upload. This one proves the seam
// between them - that the quick view, drawn through the real renderer, puts the
// fixture's pixels into the framebuffer. It is the only check that would notice a
// texture that was made but never drawn, which is the failure every part of this
// feature can produce while the code still runs.
//
// It needs a display, so on a machine without one it says so and passes. That is the
// same rule the remote suite follows with no network and the image suite follows with
// no decoder: a check that cannot run announces itself instead of pretending.
//
// The fixture is solid magenta on purpose. No colour in this interface is magenta, so
// a run of magenta pixels in the framebuffer can only have come from the file.

#include "../src/panes.cpp"

#include "image_fixtures.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(x) do { checks++; if (!(x)) { \
    failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static int skip(const char* why) {
    printf("no %s in this environment: on-screen preview checks skipped\n", why);
    printf("%d checks, %d failures\n", checks, failures);
    return 0;
}

// The row showing a name, and a shell command for the scratch directory. Both live in
// the pane suite's own helpers rather than in the app, so this file has its own.
static int idx_of(const Pane& pane, const char* name) {
    for (int i = 0; i < pane.entry_count; i++)
        if (pane.entries[i] && strcmp(pane.entries[i]->name, name) == 0) return i;
    return -1;
}

static void sh(const std::string& cmd) {
    int rc = system(cmd.c_str());
    (void)rc;
}

// A scratch directory holding one solid magenta PNG.
static std::string scratch_dir(void) {
    char tmpl[] = "/tmp/mc-gl-preview-XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (!d) return std::string();
    std::string root(d);
    std::string png = fx_png_solid(24, 24, 255, 0, 255);
    FILE* f = fopen((root + "/preview.png").c_str(), "wb");
    if (f) { fwrite(png.data(), 1, png.size(), f); fclose(f); }
    return root;
}

static bool magenta(unsigned char r, unsigned char g, unsigned char b) {
    return r > 200 && g < 60 && b > 200;
}

// The longest horizontal run of magenta in the framebuffer. The fixture is 24 pixels
// wide, so a preview that drew it whole shows a run of 24; a preview that drew it at
// half scale still shows 12, and anything that drew nothing shows none.
static int longest_magenta_run(const std::vector<unsigned char>& fb, int w, int h) {
    int best = 0;
    for (int y = 0; y < h; y++) {
        int run = 0;
        for (int x = 0; x < w; x++) {
            size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (magenta(fb[i], fb[i + 1], fb[i + 2])) {
                run++;
                if (run > best) best = run;
            } else {
                run = 0;
            }
        }
    }
    return best;
}

int main() {
    // Without a decoder there is no picture to put on a screen, and a check about
    // drawing one would be a check about nothing.
    if (!img_decoder_available()) return skip("image decoder");

    if (!SDL_Init(SDL_INIT_VIDEO)) return skip("display");

    SDL_Window* win = SDL_CreateWindow("mc-clone preview test", 900, 700,
                                       SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (!win) return skip("OpenGL-capable window");
    SDL_GLContext cx = SDL_GL_CreateContext(win);
    if (!cx) { SDL_DestroyWindow(win); SDL_Quit(); return skip("GL context"); }

    auto read_pixels = (void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))
        SDL_GL_GetProcAddress("glReadPixels");
    if (!read_pixels) {
        SDL_GL_DestroyContext(cx); SDL_DestroyWindow(win); SDL_Quit();
        return skip("glReadPixels");
    }

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplSDL3_InitForOpenGL(win, cx);
    ImGui_ImplOpenGL3_Init();

    std::string root = scratch_dir();
    if (root.empty()) {
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(ctx);
        SDL_GL_DestroyContext(cx); SDL_DestroyWindow(win); SDL_Quit();
        return skip("a scratch directory");
    }

    AppPanes app;
    net_bind(&app);
    refresh_pane(&app.left, root.c_str());
    int idx = idx_of(app.left, "preview.png");
    CHECK(idx >= 0);
    app.left.selected = idx;
    app.left.quick_view = true;          // F3, on the left pane
    app.left_focus = true;

    // Frames, with the reap between them that the real loop does. The first asks for
    // the preview, the worker answers some time later, and the texture is made by the
    // frame that draws it.
    QvImage* img = nullptr;
    int run = 0;
    for (int frame = 0; frame < 60 && run < 8; frame++) {
        SDL_PumpEvents();
        ImGui_ImplSDL3_NewFrame();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        app_panes_render(&app);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
        qv_reap(&app);

        img = qv_image_find(&app, root + "/preview.png");
        if (!img) continue;

        int fw = 0, fh = 0;
        SDL_GetWindowSize(win, &fw, &fh);
        std::vector<unsigned char> fb((size_t)fw * fh * 4, 0);
        read_pixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, fb.data());
        run = longest_magenta_run(fb, fw, fh);
    }

    CHECK(img != nullptr);
    if (img) {
        CHECK(img->fmt == IMG_PNG);
        CHECK(img->src_w == 24 && img->src_h == 24);
        // A texture made through the real renderer, not one handed back by a stub.
        CHECK(img->tex != 0);
    }
    // The pixels are on the screen. This is the check that a texture which was made
    // and never drawn would fail.
    CHECK(run >= 12);

    // And it stops being on the screen when the preview closes: the draw is tied to
    // the row, not to the cache.
    app.left.quick_view = false;
    for (int frame = 0; frame < 4; frame++) {
        SDL_PumpEvents();
        ImGui_ImplSDL3_NewFrame();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        app_panes_render(&app);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
    }
    {
        int fw = 0, fh = 0;
        SDL_GetWindowSize(win, &fw, &fh);
        std::vector<unsigned char> fb((size_t)fw * fh * 4, 0);
        read_pixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, fb.data());
        CHECK(longest_magenta_run(fb, fw, fh) == 0);
    }

    // Closing the app with a preview cached must free the texture rather than leave a
    // GL object behind in a context that is about to go away. This is what
    // app_panes_free() does; it is not called here because it ends with `delete app`
    // and this AppPanes lives on the stack.
    for (std::pair<std::string, std::unique_ptr<QvImage>>& held : app.qv_images)
        tex_free(held.second->tex);
    app.qv_images.clear();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    SDL_GL_DestroyContext(cx);
    SDL_DestroyWindow(win);
    SDL_Quit();

    sh("rm -rf '" + root + "'");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
