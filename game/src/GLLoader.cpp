#include "GLLoader.h"
#include <SDL3/SDL.h>
#include <cstdio>

#define XGL_DEFINE(ret, name, args) ret (XGL_APIENTRY* name) args = nullptr;
XGL_FUNC_LIST(XGL_DEFINE)
#undef XGL_DEFINE

bool loadGLFunctions() {
    bool ok = true;
#define XGL_LOAD(ret, name, args) \
    name = reinterpret_cast<ret (XGL_APIENTRY*) args>( \
        reinterpret_cast<void*>(SDL_GL_GetProcAddress(#name))); \
    if (name == nullptr) { std::fprintf(stderr, "GLLoader: failed to resolve %s\n", #name); ok = false; }
    XGL_FUNC_LIST(XGL_LOAD)
#undef XGL_LOAD
    return ok;
}
