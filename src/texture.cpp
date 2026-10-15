#include "texture.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

// The entry points come from SDL, which is the public way to ask a GL context for
// its own functions.
//
// ImGui's OpenGL backend has a loader of its own (`imgl3wProcs`), and going through
// it would have meant depending on a name inside one backend's private header for a
// texture this app owns. SDL is the same context, the same function pointers, and an
// API that is meant to be called from outside. It also keeps this file free of the
// renderer: src/texture.cpp can be compiled, and tested, without ImGui at all.
//
// If any of them is null - a driver that does not export it, a profile that does not
// have it - the answer is "no textures", and quick view falls back to the hex dump it
// shows today. Not a crash, and not a silent nothing.
struct TexGL {
    void (*GenTextures)(GLsizei n, GLuint* textures);
    void (*DeleteTextures)(GLsizei n, const GLuint* textures);
    void (*BindTexture)(GLenum target, GLuint texture);
    void (*TexParameteri)(GLenum target, GLenum pname, GLint param);
    void (*TexImage2D)(GLenum target, GLint level, GLint internalformat,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const void* pixels);
    void (*PixelStorei)(GLenum pname, GLint param);
};

static TexGL tex_procs(void) {
    TexGL g = {};
    g.GenTextures    = (void (*)(GLsizei, GLuint*))SDL_GL_GetProcAddress("glGenTextures");
    g.DeleteTextures = (void (*)(GLsizei, const GLuint*))SDL_GL_GetProcAddress("glDeleteTextures");
    g.BindTexture    = (void (*)(GLenum, GLuint))SDL_GL_GetProcAddress("glBindTexture");
    g.TexParameteri  = (void (*)(GLenum, GLenum, GLint))SDL_GL_GetProcAddress("glTexParameteri");
    g.TexImage2D     = (void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                 const void*))SDL_GL_GetProcAddress("glTexImage2D");
    g.PixelStorei    = (void (*)(GLenum, GLint))SDL_GL_GetProcAddress("glPixelStorei");
    return g;
}

static bool tex_have_procs(const TexGL& g) {
    return g.GenTextures && g.DeleteTextures && g.BindTexture &&
           g.TexParameteri && g.TexImage2D;
}

bool tex_available(void) {
    if (!SDL_GL_GetCurrentContext()) return false;
    return tex_have_procs(tex_procs());
}

unsigned long long tex_upload(int w, int h, const unsigned char* rgba) {
    if (w <= 0 || h <= 0 || !rgba) return 0;
    if (!SDL_GL_GetCurrentContext()) return 0;
    TexGL g = tex_procs();
    if (!tex_have_procs(g)) return 0;

    GLuint id = 0;
    g.GenTextures(1, &id);
    if (!id) return 0;

    // The shape ImGui uses for its own atlas: bilinear sampling, no tiling, and the
    // unpack state set rather than assumed, because SDL has been known to leave
    // GL_UNPACK_ROW_LENGTH set behind.
    g.BindTexture(GL_TEXTURE_2D, id);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (g.PixelStorei) {
        g.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        g.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    g.BindTexture(GL_TEXTURE_2D, 0);
    return (unsigned long long)id;
}

void tex_free(unsigned long long id) {
    if (!id) return;
    if (!SDL_GL_GetCurrentContext()) return;   // the texture died with the context
    TexGL g = tex_procs();
    if (!g.DeleteTextures) return;
    GLuint tex = (GLuint)id;
    g.DeleteTextures(1, &tex);
}
