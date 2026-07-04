#pragma once
// Hand-rolled OpenGL 3.3 core loader. Declares exactly the entry points the
// renderer uses and resolves them through SDL_GL_GetProcAddress after context
// creation. No glad/GLEW, no system GL headers (Windows only ships GL 1.1).
//
// All usage stays within the WebGL2/GLES3-compatible subset.

#include <cstddef>
#include <cstdint>

using GLenum     = unsigned int;
using GLboolean  = unsigned char;
using GLbitfield = unsigned int;
using GLbyte     = signed char;
using GLshort    = short;
using GLint      = int;
using GLsizei    = int;
using GLubyte    = unsigned char;
using GLuint     = unsigned int;
using GLfloat    = float;
using GLchar     = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr   = std::ptrdiff_t;

#ifdef _WIN32
#define XGL_APIENTRY __stdcall
#else
#define XGL_APIENTRY
#endif

// Constants (values from the GL spec)
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum GL_BLEND                = 0x0BE2;
constexpr GLenum GL_ONE                  = 1;
constexpr GLenum GL_SRC_ALPHA            = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA  = 0x0303;
constexpr GLenum GL_RGB                  = 0x1907;
constexpr GLenum GL_UNSIGNED_BYTE        = 0x1401;
constexpr GLenum GL_PACK_ALIGNMENT       = 0x0D05;
constexpr GLenum GL_STREAM_DRAW          = 0x88E0;
constexpr GLenum GL_LINES                = 0x0001;
constexpr GLenum GL_TRIANGLES            = 0x0004;
constexpr GLenum GL_FLOAT                = 0x1406;
constexpr GLenum GL_FALSE_               = 0;
constexpr GLenum GL_ARRAY_BUFFER         = 0x8892;
constexpr GLenum GL_STATIC_DRAW          = 0x88E4;
constexpr GLenum GL_FRAGMENT_SHADER      = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER        = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS       = 0x8B81;
constexpr GLenum GL_LINK_STATUS          = 0x8B82;
constexpr GLenum GL_VERSION              = 0x1F02;
constexpr GLenum GL_NO_ERROR             = 0;

#define XGL_FUNC_LIST(X) \
    X(void,           glEnable,                  (GLenum cap)) \
    X(void,           glDisable,                 (GLenum cap)) \
    X(void,           glBlendFunc,               (GLenum sfactor, GLenum dfactor)) \
    X(void,           glClear,                   (GLbitfield mask)) \
    X(void,           glClearColor,              (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void,           glViewport,                (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void,           glDrawArrays,              (GLenum mode, GLint first, GLsizei count)) \
    X(GLenum,         glGetError,                (void)) \
    X(const GLubyte*, glGetString,               (GLenum name)) \
    X(GLuint,         glCreateShader,            (GLenum type)) \
    X(void,           glShaderSource,            (GLuint shader, GLsizei count, const GLchar* const* str, const GLint* length)) \
    X(void,           glCompileShader,           (GLuint shader)) \
    X(void,           glGetShaderiv,             (GLuint shader, GLenum pname, GLint* params)) \
    X(void,           glGetShaderInfoLog,        (GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(GLuint,         glCreateProgram,           (void)) \
    X(void,           glAttachShader,            (GLuint program, GLuint shader)) \
    X(void,           glLinkProgram,             (GLuint program)) \
    X(void,           glGetProgramiv,            (GLuint program, GLenum pname, GLint* params)) \
    X(void,           glGetProgramInfoLog,       (GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(void,           glDeleteShader,            (GLuint shader)) \
    X(void,           glDeleteProgram,           (GLuint program)) \
    X(void,           glUseProgram,              (GLuint program)) \
    X(GLint,          glGetUniformLocation,      (GLuint program, const GLchar* name)) \
    X(void,           glUniform1f,               (GLint location, GLfloat v0)) \
    X(void,           glUniform1i,               (GLint location, GLint v0)) \
    X(void,           glUniform2f,               (GLint location, GLfloat v0, GLfloat v1)) \
    X(void,           glUniform3f,               (GLint location, GLfloat v0, GLfloat v1, GLfloat v2)) \
    X(void,           glPixelStorei,             (GLenum pname, GLint param)) \
    X(void,           glReadPixels,              (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels)) \
    X(void,           glUniformMatrix4fv,        (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    X(void,           glGenVertexArrays,         (GLsizei n, GLuint* arrays)) \
    X(void,           glBindVertexArray,         (GLuint array)) \
    X(void,           glDeleteVertexArrays,      (GLsizei n, const GLuint* arrays)) \
    X(void,           glGenBuffers,              (GLsizei n, GLuint* buffers)) \
    X(void,           glBindBuffer,              (GLenum target, GLuint buffer)) \
    X(void,           glBufferData,              (GLenum target, GLsizeiptr size, const void* data, GLenum usage)) \
    X(void,           glDeleteBuffers,           (GLsizei n, const GLuint* buffers)) \
    X(void,           glEnableVertexAttribArray, (GLuint index)) \
    X(void,           glVertexAttribPointer,     (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer))

#define XGL_DECLARE(ret, name, args) extern ret (XGL_APIENTRY* name) args;
XGL_FUNC_LIST(XGL_DECLARE)
#undef XGL_DECLARE

// Resolve all pointers. Returns false (and prints the missing name) on failure.
// Requires a current GL context created by SDL.
bool loadGLFunctions();
