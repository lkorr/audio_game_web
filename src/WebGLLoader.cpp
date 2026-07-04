// Web replacement for GLLoader.cpp.
//
// The native loader resolves GL 3.3 entry points through SDL_GL_GetProcAddress
// into the function pointers declared by GLLoader.h's XGL_FUNC_LIST. Under
// Emscripten we resolve the same names through emscripten_GetProcAddress (the
// WebGL2/GLES3 entry-point resolver) into those same pointers -- identical
// pattern to native, just a different resolver.
//
// GLLoader.h is included UNCHANGED. We deliberately do NOT include <GLES3/gl3.h>
// here: that header declares the same names as real functions, which would
// collide with GLLoader.h's `extern ret (*name)(args)` pointer declarations.
// Resolving by string sidesteps the collision entirely.

#include "GLLoader.h"

#include <emscripten/html5_webgl.h>   // emscripten_webgl_get_proc_address
#include <cstdio>

// Define the pointer globals declared (extern) in GLLoader.h.
#define XGL_DEFINE(ret, name, args) ret (XGL_APIENTRY* name) args = nullptr;
XGL_FUNC_LIST(XGL_DEFINE)
#undef XGL_DEFINE

bool loadGLFunctions() {
    bool ok = true;
    auto resolve = [&](void** slot, const char* name) {
        void* p = emscripten_webgl_get_proc_address(name);
        if (p == nullptr) {
            std::fprintf(stderr, "[gl] missing entry point: %s\n", name);
            ok = false;
        }
        *slot = p;
    };
#define XGL_RESOLVE(ret, name, args) resolve(reinterpret_cast<void**>(&name), #name);
    XGL_FUNC_LIST(XGL_RESOLVE)
#undef XGL_RESOLVE
    return ok;
}
