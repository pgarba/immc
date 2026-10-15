#ifndef MC_TEXTURE_H
#define MC_TEXTURE_H

// Getting a decoded picture onto the screen.
//
// This build renders ImGui through ImGui_ImplOpenGL3, and that backend takes a plain
// GLuint as the texture identifier - it binds `(GLuint)(intptr_t)pcmd->GetTexID()`.
// The pinned ImGui (1.93.0 WIP) has no texture-creation API of its own and this
// backend ships none, so a picture means a GL texture this app made.
//
// It is its own file so src/img.cpp stays free of GL and SDL and runs headless in a
// test, and so src/panes.cpp never includes a GL header. What is left here is the
// part that cannot be tested without a display, and it is deliberately small.

// A GL context is current and the entry points the upload needs were loaded. In a
// headless test, and in a build that never initialised the renderer, this is false
// and nothing below is called - which is what lets the preview pipeline be tested up
// to the pixels without a glGenTextures call landing on a context that does not exist.
bool tex_available(void);

// RGBA8 to a texture. 0 when there is no context, no entry point, or no room.
unsigned long long tex_upload(int w, int h, const unsigned char* rgba);

// Safe to call with 0, and safe to call when the context is already gone: the
// texture went with it.
void tex_free(unsigned long long id);

#endif
