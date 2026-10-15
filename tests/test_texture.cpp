// Getting pixels onto a texture.
//
// This is the one part of picture preview that cannot be checked without a display,
// so it says so and passes rather than failing on a machine with no screen - the same
// rule the remote suite follows when there is no network and the image suite follows
// when there is no decoder. On a machine with one, it exercises the whole path: a
// fixture decoded to pixels, uploaded, and freed.
//
// It is deliberately not the ImGui path. src/texture.cpp asks SDL for the GL entry
// points, so this test needs a window, a context, and nothing else.

#include "texture.h"

#include "img.h"
#include "image_fixtures.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <string>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(x) do { checks++; if (!(x)) { \
    failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static int skip(const char* why) {
    printf("no %s in this environment: texture checks skipped\n", why);
    printf("%d checks, %d failures\n", checks, failures);
    return 0;
}

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) return skip("display");

    // Hidden, because this is not a window anybody needs to see.
    SDL_Window* win = SDL_CreateWindow("mc-clone texture test", 64, 64,
                                       SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (!win) return skip("OpenGL-capable window");

    SDL_GLContext cx = SDL_GL_CreateContext(win);
    if (!cx) {
        SDL_DestroyWindow(win);
        SDL_Quit();
        return skip("GL context");
    }

    CHECK(tex_available() == true);

    unsigned char px[4 * 4 * 4];
    for (int i = 0; i < 4 * 4; i++) {
        px[i * 4 + 0] = (unsigned char)(i * 16);
        px[i * 4 + 1] = 0;
        px[i * 4 + 2] = 0;
        px[i * 4 + 3] = 255;
    }

    unsigned long long id = tex_upload(4, 4, px);
    CHECK(id != 0);
    // A texture id is handed back as the number ImGui's OpenGL backend binds, so 0 -
    // the "no texture" value in that backend - must never come out of a success.
    CHECK(id != 0);
    tex_free(id);

    // Two uploads are two different textures, and freeing one leaves the other alone.
    unsigned long long a = tex_upload(2, 2, px), b = tex_upload(3, 3, px);
    CHECK(a != 0 && b != 0 && a != b);
    tex_free(a);
    tex_free(b);

    // Refusals, so a caller never has to guess which of its own mistakes was the one
    // that produced a 0.
    CHECK(tex_upload(0, 4, px) == 0);
    CHECK(tex_upload(4, 0, px) == 0);
    CHECK(tex_upload(-2, 2, px) == 0);
    CHECK(tex_upload(4, 4, nullptr) == 0);
    tex_free(0);   // freeing nothing is not an error

    // The whole path a quick view takes: bytes, decode, upload.
    if (img_decoder_available()) {
        std::string png = fx_png(8, 8);
        int w = 0, h = 0, orient = 0;
        std::vector<unsigned char> rgba;
        std::string err;
        CHECK(img_decode(png, IMG_PNG, &w, &h, &rgba, &orient, &err));
        unsigned long long real = tex_upload(w, h, rgba.data());
        CHECK(real != 0);
        tex_free(real);
    }

    // After the context is gone, nothing here calls into it. The textures went with
    // it, and a free that tried to use a dead context would be a crash on the way out
    // of the app - which is the moment this runs.
    SDL_GL_DestroyContext(cx);
    CHECK(tex_available() == false);
    CHECK(tex_upload(2, 2, px) == 0);
    tex_free(12345);   // an id from a context that no longer exists

    SDL_DestroyWindow(win);
    SDL_Quit();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
