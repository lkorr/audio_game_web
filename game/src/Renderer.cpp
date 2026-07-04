#include "Renderer.h"
#include "GLLoader.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;

const char* kLineVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uMVP;
uniform vec3 uOffset;   // world-space offset (dev preview shapes; 0 otherwise)
uniform float uScale;   // uniform scale (paint brush ring; 1 otherwise)
out vec3 vWorld;
void main() {
    vWorld = aPos * uScale + uOffset;
    gl_Position = uMVP * vec4(vWorld, 1.0);
}
)";

const char* kLineFS = R"(#version 330 core
in vec3 vWorld;
uniform vec3 uColor;
uniform float uBrightness;
uniform vec3 uPlayerPos;
uniform float uFadeRadius;    // <= 0: no distance fade
uniform vec3 uFlashPos;       // world-space contact point of the last bump
uniform float uFlashAmount;   // 0..1 flash intensity (decays on game thread)
uniform float uFlashRadius;   // falloff radius around the contact point
out vec4 fragColor;
void main() {
    float a = 1.0;
    if (uFadeRadius > 0.0) {
        float d = distance(vWorld, uPlayerPos);
        a = clamp(1.0 - d / uFadeRadius, 0.0, 1.0);
        a *= a;
    }
    // Localized bump flash: brighten only the surface near the touch point.
    float bright = uBrightness;
    if (uFlashAmount > 0.001) {
        float fd = distance(vWorld, uFlashPos);
        float falloff = clamp(1.0 - fd / uFlashRadius, 0.0, 1.0);
        bright += uFlashAmount * falloff * falloff;
    }
    fragColor = vec4(uColor * bright * a, 1.0);
}
)";

// Solid ("emergent") objects: additive filled triangles whose brightness comes
// from proximity to the player + a reveal-on-sound level, so shapes surface from
// the dark when you approach or when they sound. Same no-depth additive pipeline
// as the lines -- black (brightness ~0) is a no-op, so distant/silent objects
// vanish. A per-vertex shade lets caps read slightly darker than sides for form.
const char* kObjVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aShade;
uniform mat4 uMVP;
out vec3 vWorld;
out float vShade;
void main() {
    vWorld = aPos;
    vShade = aShade;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* kObjFS = R"(#version 330 core
in vec3 vWorld;
in float vShade;
uniform vec3 uColor;
uniform vec3 uPlayerPos;
uniform float uRevealRadius;  // objects fade to black past this distance
uniform float uAudioLevel;    // 0..1 reveal-on-sound floor
uniform vec3 uFlashPos;
uniform float uFlashAmount;
uniform float uFlashRadius;
out vec4 fragColor;
void main() {
    // Proximity term: bright at the player, smooth to 0 at uRevealRadius.
    float prox = 0.0;
    if (uRevealRadius > 0.0) {
        float d = distance(vWorld, uPlayerPos);
        prox = clamp(1.0 - d / uRevealRadius, 0.0, 1.0);
        prox *= prox;
    }
    float bright = max(prox, uAudioLevel);
    if (uFlashAmount > 0.001) {
        float fd = distance(vWorld, uFlashPos);
        float falloff = clamp(1.0 - fd / uFlashRadius, 0.0, 1.0);
        bright += uFlashAmount * falloff * falloff;
    }
    fragColor = vec4(uColor * bright * vShade, 1.0);
}
)";

const char* kGlowVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aAlpha;
uniform mat4 uMVP;
uniform vec3 uOffset;
out vec3 vWorld;
out float vAlpha;
void main() {
    vWorld = aPos + uOffset;
    vAlpha = aAlpha;
    gl_Position = uMVP * vec4(vWorld, 1.0);
}
)";

const char* kGlowFS = R"(#version 330 core
in vec3 vWorld;
in float vAlpha;
uniform vec3 uColor;
uniform float uIntensity;
uniform vec3 uPlayerPos;
uniform float uFadeRadius;   // <= 0: no distance fade (confines the lit pool)
uniform int uPattern;        // 0 solid, 1 hatch, 2 checker, 3 dots, 4 stripes
out vec4 fragColor;

// World-space floor pattern so zones differ by TEXTURE, not just hue (readable
// for colorblind players). Returns a brightness multiplier in ~[0.5, 1.3].
float pattern(int kind, vec2 p) {
    if (kind == 1) {                 // fine cross-hatch (1 m grid of thin lines)
        vec2 g = abs(fract(p) - 0.5);
        float line = step(0.42, max(g.x, g.y));
        return mix(0.7, 1.25, line);
    } else if (kind == 2) {          // 1 m checkerboard
        float c = mod(floor(p.x) + floor(p.y), 2.0);
        return mix(0.65, 1.2, c);
    } else if (kind == 3) {          // dot stipple (0.7 m spacing)
        vec2 c = fract(p * 1.4) - 0.5;
        float dot = 1.0 - smoothstep(0.16, 0.24, length(c));
        return mix(0.7, 1.3, dot);
    } else if (kind == 4) {          // diagonal stripes
        float s = abs(fract((p.x + p.y) * 0.6) - 0.5);
        return mix(0.7, 1.25, step(0.25, s));
    }
    return 1.0;                      // 0: solid (creek band, player disc)
}

void main() {
    float a = vAlpha;
    if (uFadeRadius > 0.0) {
        float d = distance(vWorld, uPlayerPos);
        float f = clamp(1.0 - d / uFadeRadius, 0.0, 1.0);
        a *= f * f;
    }
    float pat = pattern(uPattern, vWorld.xy);
    fragColor = vec4(uColor * uIntensity * a * pat, 1.0);
}
)";

const char* kOverlayVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;    // pixels, origin top-left
layout(location = 1) in vec4 aColor;
uniform vec2 uScreen;                 // framebuffer size in pixels
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0,
                       1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}
)";

const char* kOverlayFS = R"(#version 330 core
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";

// Classic 5x7 pixel font, ASCII 32..95 (space .. underscore). Each glyph is
// 7 rows, 5 bits per row, MSB = leftmost column. Lowercase is mapped to
// uppercase at draw time; anything else falls back to '?'.
constexpr unsigned char kFont5x7[64][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, // !
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, // "
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, // #
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, // $
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, // %
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, // &
    {0x0C,0x04,0x08,0x00,0x00,0x00,0x00}, // '
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, // (
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, // )
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, // *
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // +
    {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, // ,
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // .
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, // /
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, // :
    {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}, // ;
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, // <
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, // =
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, // >
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, // ?
    {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, // @
    {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}, // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, // W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, // [
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, // backslash
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, // ]
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // _
};

const unsigned char* glyphFor(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c < 32 || c > 95) c = '?';
    return kFont5x7[c - 32];
}

#ifdef __EMSCRIPTEN__
// WebGL2 speaks GLSL ES 3.00, not desktop GLSL 3.30. The two are source-
// compatible for everything these shaders use (layout locations, in/out,
// mat4) EXCEPT the version directive and the mandatory fragment-shader
// precision qualifiers. Rewrite the "#version 330 core" prologue that every
// shader literal starts with into an ES 3.00 prologue; the shader bodies are
// untouched. Kept local to the web build so the native path is unaffected.
std::string toGLSLES(const char* src) {
    const char* kDesktop = "#version 330 core";
    std::string s(src);
    const size_t pos = s.find(kDesktop);
    const std::string esHeader =
        "#version 300 es\nprecision highp float;\nprecision highp int;";
    if (pos != std::string::npos)
        s.replace(pos, std::strlen(kDesktop), esHeader);
    else
        s = esHeader + "\n" + s;   // defensive: no version line found
    return s;
}
#endif

unsigned compileShader(GLenum type, const char* src) {
    const GLuint sh = glCreateShader(type);
#ifdef __EMSCRIPTEN__
    const std::string translated = toGLSLES(src);
    const char* out = translated.c_str();
    glShaderSource(sh, 1, &out, nullptr);
#else
    glShaderSource(sh, 1, &src, nullptr);
#endif
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Shader compile error: %s\n", log);
        return 0;
    }
    return sh;
}

unsigned linkProgram(const char* vs, const char* fs) {
    const GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    const GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (v == 0 || f == 0) return 0;
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Program link error: %s\n", log);
        return 0;
    }
    return prog;
}

void pushLine(std::vector<float>& v, const glm::vec3& a, const glm::vec3& b) {
    v.insert(v.end(), { a.x, a.y, a.z, b.x, b.y, b.z });
}

void appendCircle(std::vector<float>& v, const glm::vec3& center, float r, int segs) {
    for (int i = 0; i < segs; ++i) {
        const float a0 = 2.0f * kPi * static_cast<float>(i) / segs;
        const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / segs;
        pushLine(v, center + glm::vec3(r * std::cos(a0), r * std::sin(a0), 0.0f),
                    center + glm::vec3(r * std::cos(a1), r * std::sin(a1), 0.0f));
    }
}

void appendCylinder(std::vector<float>& v, const glm::vec2& c, float r, float z0, float z1) {
    constexpr int kSegs = 14;
    appendCircle(v, { c.x, c.y, z0 }, r, kSegs);
    appendCircle(v, { c.x, c.y, z1 }, r, kSegs);
    for (int i = 0; i < 6; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / 6.0f;
        const glm::vec3 off(r * std::cos(a), r * std::sin(a), 0.0f);
        pushLine(v, glm::vec3(c.x, c.y, z0) + off, glm::vec3(c.x, c.y, z1) + off);
    }
}

void appendCone(std::vector<float>& v, const glm::vec2& c, float baseZ, float r, float height) {
    constexpr int kSegs = 12;
    appendCircle(v, { c.x, c.y, baseZ }, r, kSegs);
    const glm::vec3 apex(c.x, c.y, baseZ + height);
    for (int i = 0; i < 6; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / 6.0f;
        pushLine(v, glm::vec3(c.x + r * std::cos(a), c.y + r * std::sin(a), baseZ), apex);
    }
}

void appendBox(std::vector<float>& v, const glm::vec3& center, const glm::vec3& half) {
    const glm::vec3 mn = center - half, mx = center + half;
    const glm::vec3 p[8] = {
        { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
        { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z }, { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
    };
    const int e[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7} };
    for (auto& edge : e) pushLine(v, p[edge[0]], p[edge[1]]);
}

void appendDiamond(std::vector<float>& v, const glm::vec3& c, float r) {
    const glm::vec3 px(r, 0, 0), py(0, r, 0), pz(0, 0, r);
    const glm::vec3 pts[6] = { c + px, c - px, c + py, c - py, c + pz, c - pz };
    const int e[12][2] = { {0,2},{2,1},{1,3},{3,0}, {0,4},{2,4},{1,4},{3,4}, {0,5},{2,5},{1,5},{3,5} };
    for (auto& edge : e) pushLine(v, pts[edge[0]], pts[edge[1]]);
}

// ---- Solid (filled triangle) builders: layout pos3 + shade1. `shade` tints
//      faces so form reads in the dark (sides brighter than caps). ----
void pushTri(std::vector<float>& v, const glm::vec3& a, const glm::vec3& b,
             const glm::vec3& c, float shade) {
    v.insert(v.end(), { a.x, a.y, a.z, shade, b.x, b.y, b.z, shade, c.x, c.y, c.z, shade });
}

void appendSolidCylinder(std::vector<float>& v, const glm::vec2& c, float r,
                         float z0, float z1) {
    constexpr int kSegs = 18;
    const glm::vec3 top(c.x, c.y, z1);
    for (int i = 0; i < kSegs; ++i) {
        const float a0 = 2.0f * kPi * static_cast<float>(i) / kSegs;
        const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / kSegs;
        const glm::vec3 d0(std::cos(a0), std::sin(a0), 0.0f);
        const glm::vec3 d1(std::cos(a1), std::sin(a1), 0.0f);
        const glm::vec3 b0(c.x + r * d0.x, c.y + r * d0.y, z0);
        const glm::vec3 b1(c.x + r * d1.x, c.y + r * d1.y, z0);
        const glm::vec3 t0(c.x + r * d0.x, c.y + r * d0.y, z1);
        const glm::vec3 t1(c.x + r * d1.x, c.y + r * d1.y, z1);
        pushTri(v, b0, b1, t1, 1.0f);   // side quad
        pushTri(v, b0, t1, t0, 1.0f);
        pushTri(v, top, t0, t1, 0.7f);  // top cap
    }
}

void appendSolidCone(std::vector<float>& v, const glm::vec2& c, float baseZ,
                     float r, float height) {
    constexpr int kSegs = 18;
    const glm::vec3 apex(c.x, c.y, baseZ + height);
    for (int i = 0; i < kSegs; ++i) {
        const float a0 = 2.0f * kPi * static_cast<float>(i) / kSegs;
        const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / kSegs;
        const glm::vec3 p0(c.x + r * std::cos(a0), c.y + r * std::sin(a0), baseZ);
        const glm::vec3 p1(c.x + r * std::cos(a1), c.y + r * std::sin(a1), baseZ);
        pushTri(v, p0, p1, apex, 0.9f);
    }
}

void appendSolidBox(std::vector<float>& v, const glm::vec3& center, const glm::vec3& half) {
    const glm::vec3 mn = center - half, mx = center + half;
    const glm::vec3 p[8] = {
        { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z }, { mx.x, mx.y, mn.z }, { mn.x, mx.y, mn.z },
        { mn.x, mn.y, mx.z }, { mx.x, mn.y, mx.z }, { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
    };
    // 6 faces (2 tris each); alternating shade so adjacent faces separate.
    const int f[6][4] = { {0,1,2,3}, {4,5,6,7}, {0,1,5,4}, {2,3,7,6}, {1,2,6,5}, {0,3,7,4} };
    const float shade[6] = { 0.7f, 1.0f, 0.85f, 0.85f, 0.9f, 0.9f };
    for (int i = 0; i < 6; ++i) {
        pushTri(v, p[f[i][0]], p[f[i][1]], p[f[i][2]], shade[i]);
        pushTri(v, p[f[i][0]], p[f[i][2]], p[f[i][3]], shade[i]);
    }
}

} // namespace

bool Renderer::init(const Scene& scene) {
    lineProgram_ = linkProgram(kLineVS, kLineFS);
    glowProgram_ = linkProgram(kGlowVS, kGlowFS);
    objProgram_ = linkProgram(kObjVS, kObjFS);
    overlayProgram_ = linkProgram(kOverlayVS, kOverlayFS);
    if (lineProgram_ == 0 || glowProgram_ == 0 || objProgram_ == 0 ||
        overlayProgram_ == 0) return false;
    uOverlayScreen_ = glGetUniformLocation(overlayProgram_, "uScreen");

    // ---- Overlay: dynamic quad buffer, (x y) + (r g b a) per vertex ----
    glGenVertexArrays(1, &overlayVAO_);
    glBindVertexArray(overlayVAO_);
    glGenBuffers(1, &overlayVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, overlayVBO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE_, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE_, 6 * sizeof(float),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    glBindVertexArray(0);

    uLineMVP_ = glGetUniformLocation(lineProgram_, "uMVP");
    uLineColor_ = glGetUniformLocation(lineProgram_, "uColor");
    uLineBright_ = glGetUniformLocation(lineProgram_, "uBrightness");
    uLinePlayer_ = glGetUniformLocation(lineProgram_, "uPlayerPos");
    uLineFade_ = glGetUniformLocation(lineProgram_, "uFadeRadius");
    uLineFlashPos_ = glGetUniformLocation(lineProgram_, "uFlashPos");
    uLineFlashAmount_ = glGetUniformLocation(lineProgram_, "uFlashAmount");
    uLineFlashRadius_ = glGetUniformLocation(lineProgram_, "uFlashRadius");
    uLineOffset_ = glGetUniformLocation(lineProgram_, "uOffset");
    uLineScale_ = glGetUniformLocation(lineProgram_, "uScale");
    uGlowMVP_ = glGetUniformLocation(glowProgram_, "uMVP");
    uGlowOffset_ = glGetUniformLocation(glowProgram_, "uOffset");
    uGlowColor_ = glGetUniformLocation(glowProgram_, "uColor");
    uGlowIntensity_ = glGetUniformLocation(glowProgram_, "uIntensity");
    uGlowPlayer_ = glGetUniformLocation(glowProgram_, "uPlayerPos");
    uGlowFade_ = glGetUniformLocation(glowProgram_, "uFadeRadius");
    uGlowPattern_ = glGetUniformLocation(glowProgram_, "uPattern");
    uObjMVP_ = glGetUniformLocation(objProgram_, "uMVP");
    uObjColor_ = glGetUniformLocation(objProgram_, "uColor");
    uObjPlayer_ = glGetUniformLocation(objProgram_, "uPlayerPos");
    uObjReveal_ = glGetUniformLocation(objProgram_, "uRevealRadius");
    uObjAudio_ = glGetUniformLocation(objProgram_, "uAudioLevel");
    uObjFlashPos_ = glGetUniformLocation(objProgram_, "uFlashPos");
    uObjFlashAmount_ = glGetUniformLocation(objProgram_, "uFlashAmount");
    uObjFlashRadius_ = glGetUniformLocation(objProgram_, "uFlashRadius");

    // ---- Line geometry: VAO/VBO created once, data uploaded (and re-uploaded
    //      on dev-mode scene edits) by uploadSceneGeometry ----
    glGenVertexArrays(1, &lineVAO_);
    glBindVertexArray(lineVAO_);
    glGenBuffers(1, &lineVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_, 3 * sizeof(float), nullptr);
    uploadSceneGeometry(scene);

    // ---- Solid object geometry (triangles, position + per-vertex shade) ----
    glGenVertexArrays(1, &objVAO_);
    glBindVertexArray(objVAO_);
    glGenBuffers(1, &objVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, objVBO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE_, 4 * sizeof(float),
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    uploadObjectGeometry(scene);

    // ---- Glow geometry (triangles, position + per-vertex alpha) ----
    std::vector<float> glow;  // x y z a
    auto pushGlowVert = [&](float x, float y, float z, float a) {
        glow.insert(glow.end(), { x, y, z, a });
    };
    // Player disc: fan around origin (offset via uniform), radius 1 m
    discFirst_ = 0;
    {
        constexpr int kSegs = 24;
        for (int i = 0; i < kSegs; ++i) {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / kSegs;
            const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / kSegs;
            pushGlowVert(0.0f, 0.0f, 0.01f, 1.0f);
            pushGlowVert(std::cos(a0), std::sin(a0), 0.01f, 0.0f);
            pushGlowVert(std::cos(a1), std::sin(a1), 0.01f, 0.0f);
        }
        discCount_ = static_cast<int>(glow.size() / 4);
    }
    // Creek band: strip with bright centerline fading to the edges
    creekFirst_ = static_cast<int>(glow.size() / 4);
    {
        const float h = Scene::kHalfExtent;
        const float yc = Scene::kCreekY;
        const float yw = Scene::kCreekHalfWidth;
        // two quads: [yc-yw .. yc] and [yc .. yc+yw], center alpha 1
        const float z = 0.005f;
        auto quad = [&](float y0, float a0, float y1, float a1) {
            pushGlowVert(-h, y0, z, a0); pushGlowVert(h, y0, z, a0); pushGlowVert(h, y1, z, a1);
            pushGlowVert(-h, y0, z, a0); pushGlowVert(h, y1, z, a1); pushGlowVert(-h, y1, z, a1);
        };
        quad(yc - yw, 0.0f, yc, 1.0f);
        quad(yc, 1.0f, yc + yw, 0.0f);
        creekCount_ = static_cast<int>(glow.size() / 4) - creekFirst_;
    }

    glGenVertexArrays(1, &glowVAO_);
    glBindVertexArray(glowVAO_);
    glGenBuffers(1, &glowVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, glowVBO_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(glow.size() * sizeof(float)),
                 glow.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE_, 4 * sizeof(float),
                          reinterpret_cast<const void*>(3 * sizeof(float)));

    // ---- Painted-floor mesh (same layout as glow: pos3 + alpha) ----
    glGenVertexArrays(1, &floorVAO_);
    glBindVertexArray(floorVAO_);
    glGenBuffers(1, &floorVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, floorVBO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE_, 4 * sizeof(float),
                          reinterpret_cast<const void*>(3 * sizeof(float)));

    // ---- Floor boundary lines (pos3, drawn with the line program) ----
    glGenVertexArrays(1, &floorEdgeVAO_);
    glBindVertexArray(floorEdgeVAO_);
    glGenBuffers(1, &floorEdgeVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, floorEdgeVBO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE_, 3 * sizeof(float), nullptr);

    uploadFloorGeometry(scene);

    glBindVertexArray(0);
    return true;
}

glm::vec3 Renderer::floorColor(int mat) {
    static const glm::vec3 kPalette[8] = {
        { 0.28f, 0.75f, 0.34f },  // 0 grass green
        { 0.62f, 0.68f, 0.82f },  // 1 stone blue-grey
        { 0.92f, 0.66f, 0.26f },  // 2 wood/sand gold
        { 0.72f, 0.38f, 0.95f },  // 3 cavern purple
        { 0.25f, 0.90f, 0.82f },  // 4 teal
        { 0.97f, 0.40f, 0.40f },  // 5 red
        { 0.40f, 0.60f, 1.00f },  // 6 blue
        { 0.82f, 0.92f, 0.38f },  // 7 lime
    };
    return kPalette[((mat % 8) + 8) % 8];
}

// Each material gets a distinct floor texture so zones read without relying on
// color alone. Indices match kGlowFS pattern(): 0 solid, 1 hatch, 2 checker,
// 3 dots, 4 stripes. Cycles for materials beyond the first few.
int Renderer::floorPattern(int mat) {
    static const int kPatterns[8] = { 1, 0, 4, 2, 3, 4, 2, 3 };
    return kPatterns[((mat % 8) + 8) % 8];
}

void Renderer::uploadFloorGeometry(const Scene& scene) {
    // One quad per run of same-material cells per row, one draw range per
    // material (the glow shader colors + patterns per draw call). Material 0 is
    // painted too, so the base ground reads as its own zone (grass), not as
    // bare grid. A separate line list traces the boundaries between different
    // materials so zone edges are crisp.
    const float z = 0.003f;    // below the creek band (0.005), above the grid
    const float ez = 0.006f;   // boundary lines sit just above the floor fill
    std::vector<float> verts;  // x y z a
    std::vector<float> edges;  // x y z  (boundary line segments)
    floorRanges_.clear();
    auto pushQuad = [&](float x0, float y0, float x1, float y1) {
        const float q[6][2] = {
            { x0, y0 }, { x1, y0 }, { x1, y1 },
            { x0, y0 }, { x1, y1 }, { x0, y1 },
        };
        for (const auto& p : q)
            verts.insert(verts.end(), { p[0], p[1], z, 1.0f });
    };
    auto pushEdge = [&](float x0, float y0, float x1, float y1) {
        edges.insert(edges.end(), { x0, y0, ez, x1, y1, ez });
    };
    const float base = -Scene::kHalfExtent;
    auto cellX = [&](int i) { return base + static_cast<float>(i) * Scene::kCellSize; };
    auto cellY = [&](int j) { return base + static_cast<float>(j) * Scene::kCellSize; };

    int maxMat = 0;
    for (const uint8_t c : scene.floor) maxMat = std::max(maxMat, static_cast<int>(c));
    for (int mat = 0; mat <= maxMat; ++mat) {
        FloorRange range;
        range.first = static_cast<int>(verts.size() / 4);
        range.material = mat;
        for (int j = 0; j < Scene::kGridN; ++j) {
            const float y0 = cellY(j);
            int i = 0;
            while (i < Scene::kGridN) {
                if (scene.cell(i, j) != mat) { ++i; continue; }
                int run = 1;
                while (i + run < Scene::kGridN && scene.cell(i + run, j) == mat) ++run;
                pushQuad(cellX(i), y0, cellX(i + run), y0 + Scene::kCellSize);
                i += run;
            }
        }
        range.count = static_cast<int>(verts.size() / 4) - range.first;
        if (range.count > 0) floorRanges_.push_back(range);
    }

    // Boundary edges: a segment on any cell face where the neighbor differs.
    // Treat off-grid as material 0 so the outer edge of painted zones is drawn.
    auto matAt = [&](int i, int j) -> int {
        if (i < 0 || j < 0 || i >= Scene::kGridN || j >= Scene::kGridN) return 0;
        return scene.cell(i, j);
    };
    for (int j = 0; j < Scene::kGridN; ++j)
        for (int i = 0; i < Scene::kGridN; ++i) {
            const int m = matAt(i, j);
            if (matAt(i + 1, j) != m)   // vertical face at right edge of cell
                pushEdge(cellX(i + 1), cellY(j), cellX(i + 1), cellY(j + 1));
            if (matAt(i, j + 1) != m)   // horizontal face at top edge of cell
                pushEdge(cellX(i), cellY(j + 1), cellX(i + 1), cellY(j + 1));
        }
    floorEdgeCount_ = static_cast<int>(edges.size() / 3);

    glBindVertexArray(floorVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, floorVBO_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);

    glBindVertexArray(floorEdgeVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, floorEdgeVBO_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(edges.size() * sizeof(float)),
                 edges.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
}

void Renderer::rebuildFloor(const Scene& scene) {
    uploadFloorGeometry(scene);
}

void Renderer::uploadObjectGeometry(const Scene& scene) {
    // Solid tree/boulder triangles. Layout pos3 + shade1; one draw range per
    // object so the object shader can key proximity/reveal/flash per object.
    std::vector<float> verts;
    auto beginRange = [&]() { return static_cast<int>(verts.size() / 4); };
    objRanges_.clear();

    for (size_t i = 0; i < scene.trees.size(); ++i) {
        const Tree& t = scene.trees[i];
        DrawRange r;
        r.first = beginRange();
        appendSolidCylinder(verts, t.pos, t.trunkRadius, 0.0f, t.trunkHeight);
        appendSolidCone(verts, t.pos, t.trunkHeight, t.canopyRadius, t.canopyHeight);
        r.count = beginRange() - r.first;
        r.color = { 0.30f, 0.95f, 0.42f };  // trees glow green
        r.treeIndex = static_cast<int>(i);
        objRanges_.push_back(r);
    }
    for (size_t i = 0; i < scene.boulders.size(); ++i) {
        const Boulder& b = scene.boulders[i];
        DrawRange r;
        r.first = beginRange();
        appendSolidBox(verts, b.center, b.half);
        r.count = beginRange() - r.first;
        r.color = { 1.0f, 0.5f, 0.28f };  // boulders glow orange
        r.boulderIndex = static_cast<int>(i);
        objRanges_.push_back(r);
    }

    glBindVertexArray(objVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, objVBO_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
}

void Renderer::uploadSceneGeometry(const Scene& scene) {
    std::vector<float> verts;
    auto beginRange = [&]() { return static_cast<int>(verts.size() / 3); };
    ranges_.clear();

    {
        // Creek edge lines + emitter markers, static dim range
        DrawRange r;
        r.first = beginRange();
        const float h = Scene::kHalfExtent;
        pushLine(verts, { -h, Scene::kCreekY - Scene::kCreekHalfWidth, 0.01f },
                        { h, Scene::kCreekY - Scene::kCreekHalfWidth, 0.01f });
        pushLine(verts, { -h, Scene::kCreekY + Scene::kCreekHalfWidth, 0.01f },
                        { h, Scene::kCreekY + Scene::kCreekHalfWidth, 0.01f });
        for (const Emitter& e : scene.emitters)
            appendDiamond(verts, e.pos, 0.15f);
        r.count = beginRange() - r.first;
        r.color = { 0.5f, 0.75f, 1.0f };
        ranges_.push_back(r);
    }
    // Ground grid (distance-faded in the shader)
    gridFirst_ = beginRange();
    {
        const float h = Scene::kHalfExtent;
        for (int i = -static_cast<int>(h); i <= static_cast<int>(h); ++i) {
            const float f = static_cast<float>(i);
            pushLine(verts, { f, -h, 0.0f }, { f, h, 0.0f });
            pushLine(verts, { -h, f, 0.0f }, { h, f, 0.0f });
        }
    }
    gridCount_ = beginRange() - gridFirst_;

    // Dev preview unit shapes at the origin, positioned at draw time via
    // uOffset: default-dimension tree, default boulder, emitter diamond.
    previewFirst_[0] = beginRange();
    {
        const Tree t{};  // default dims
        appendCylinder(verts, { 0.0f, 0.0f }, t.trunkRadius, 0.0f, t.trunkHeight);
        appendCone(verts, { 0.0f, 0.0f }, t.trunkHeight, t.canopyRadius, t.canopyHeight);
    }
    previewCount_[0] = beginRange() - previewFirst_[0];
    previewFirst_[1] = beginRange();
    appendBox(verts, { 0.0f, 0.0f, 0.45f }, { 0.55f, 0.5f, 0.45f });
    previewCount_[1] = beginRange() - previewFirst_[1];
    previewFirst_[2] = beginRange();
    appendDiamond(verts, { 0.0f, 0.0f, 0.0f }, 0.2f);
    previewCount_[2] = beginRange() - previewFirst_[2];
    // Unit ground circle (paint brush ring; z = 0 so uScale leaves it on the
    // ground -- the draw offset lifts it slightly)
    previewFirst_[3] = beginRange();
    appendCircle(verts, { 0.0f, 0.0f, 0.0f }, 1.0f, 48);
    previewCount_[3] = beginRange() - previewFirst_[3];

    glBindVertexArray(lineVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
}

void Renderer::rebuildScene(const Scene& scene) {
    uploadSceneGeometry(scene);
    uploadObjectGeometry(scene);
    uploadFloorGeometry(scene);
}

void Renderer::shutdown() {
    if (lineVBO_) glDeleteBuffers(1, &lineVBO_);
    if (objVBO_) glDeleteBuffers(1, &objVBO_);
    if (glowVBO_) glDeleteBuffers(1, &glowVBO_);
    if (floorVBO_) glDeleteBuffers(1, &floorVBO_);
    if (floorEdgeVBO_) glDeleteBuffers(1, &floorEdgeVBO_);
    if (overlayVBO_) glDeleteBuffers(1, &overlayVBO_);
    if (lineVAO_) glDeleteVertexArrays(1, &lineVAO_);
    if (objVAO_) glDeleteVertexArrays(1, &objVAO_);
    if (glowVAO_) glDeleteVertexArrays(1, &glowVAO_);
    if (floorVAO_) glDeleteVertexArrays(1, &floorVAO_);
    if (floorEdgeVAO_) glDeleteVertexArrays(1, &floorEdgeVAO_);
    if (overlayVAO_) glDeleteVertexArrays(1, &overlayVAO_);
    if (lineProgram_) glDeleteProgram(lineProgram_);
    if (objProgram_) glDeleteProgram(objProgram_);
    if (glowProgram_) glDeleteProgram(glowProgram_);
    if (overlayProgram_) glDeleteProgram(overlayProgram_);
}

void Renderer::drawOverlay(const std::vector<OverlayRect>& rects,
                           const std::vector<OverlayText>& texts,
                           int fbWidth, int fbHeight) {
    if (rects.empty() && texts.empty()) return;

    overlayVerts_.clear();
    auto pushQuad = [this](float x, float y, float w, float h, const glm::vec4& c) {
        const float v[6][2] = {
            { x, y }, { x + w, y }, { x + w, y + h },
            { x, y }, { x + w, y + h }, { x, y + h },
        };
        for (const auto& p : v)
            overlayVerts_.insert(overlayVerts_.end(), { p[0], p[1], c.r, c.g, c.b, c.a });
    };

    for (const OverlayRect& r : rects)
        pushQuad(r.x, r.y, r.w, r.h, r.color);

    for (const OverlayText& t : texts) {
        float cx = t.x;
        for (char ch : t.text) {
            const unsigned char* g = glyphFor(ch);
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (g[row] & (0x10 >> col))
                        pushQuad(cx + static_cast<float>(col) * t.scale,
                                 t.y + static_cast<float>(row) * t.scale,
                                 t.scale, t.scale, t.color);
            cx += 6.0f * t.scale;
        }
    }

    // Standard alpha blending: dark panels must dim the scene behind them
    // (the world pass uses additive, where black is a no-op).
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(overlayProgram_);
    glUniform2f(uOverlayScreen_, static_cast<float>(fbWidth), static_cast<float>(fbHeight));
    glBindVertexArray(overlayVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, overlayVBO_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(overlayVerts_.size() * sizeof(float)),
                 overlayVerts_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(overlayVerts_.size() / 6));

    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void Renderer::draw(const Scene& scene, const glm::vec3& eyePos, float yaw, float pitch,
                    int fbWidth, int fbHeight, float timeSeconds,
                    float groundGlowBoost, VisualMode mode,
                    const DevPreview* preview) {
    const bool fullBright = (mode == VisualMode::FullBright);
    const bool blind = (mode == VisualMode::Blind);
    // Default (Lit) shows solid objects + floor across the whole map. Blind mode
    // is near-total darkness: only a small pool of ground around the player is
    // lit and objects surface from black by proximity + reveal-on-sound.
    // FullBright is the debug lift (same reach as Lit, brighter).
    // In Lit/FullBright the reveal/fade radii are disabled (0 = no fade), so
    // the pool constants below only bite in Blind mode.
    constexpr float kLitPoolRadius = 1.4f;    // lit ground disc around the feet
    constexpr float kRevealRadius = 2.0f;     // objects visible within this range
    const float gridFade = blind ? kLitPoolRadius : 0.0f;   // 0 = whole map
    const float revealRad = blind ? kRevealRadius : 0.0f;   // 0 = no reveal gate
    const float floorFade = blind ? kLitPoolRadius : 0.0f;
    glViewport(0, 0, fbWidth, fbHeight);
    glClearColor(0.0196f, 0.0196f, 0.0313f, 1.0f);  // #050508
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);  // additive: draw order independent

    // World is X-right / Y-forward / Z-up; lookAt with up = +Z is the single
    // fixed rotation into GL's Y-up clip space.
    const float cp = std::cos(pitch);
    const glm::vec3 fwd{ -std::sin(yaw) * cp, std::cos(yaw) * cp, std::sin(pitch) };
    const glm::mat4 view = glm::lookAt(eyePos, eyePos + fwd, glm::vec3(0, 0, 1));
    const glm::mat4 proj = glm::perspective(glm::radians(70.0f),
        static_cast<float>(fbWidth) / static_cast<float>(std::max(fbHeight, 1)), 0.05f, 200.0f);
    const glm::mat4 mvp = proj * view;

    glUseProgram(lineProgram_);
    glUniformMatrix4fv(uLineMVP_, 1, GL_FALSE_, glm::value_ptr(mvp));
    glUniform3f(uLinePlayer_, eyePos.x, eyePos.y, 0.0f);
    glUniform1f(uLineFlashRadius_, 1.1f);
    glUniform3f(uLineOffset_, 0.0f, 0.0f, 0.0f);
    glUniform1f(uLineScale_, 1.0f);
    glBindVertexArray(lineVAO_);

    // Grid: the proprioceptive ground reference. Lit/FullBright show the whole
    // grid; Blind confines it to a small lit pool around the player (fade =
    // kLitPoolRadius) so only the floor you are walking on is visible.
    glUniform1f(uLineFade_, gridFade);
    glUniform1f(uLineFlashAmount_, 0.0f);
    glUniform3f(uLineColor_, 0.45f, 0.5f, 0.6f);
    glUniform1f(uLineBright_, fullBright ? 0.6f : blind ? 0.16f : 0.28f);
    glDrawArrays(GL_LINES, gridFirst_, gridCount_);

    // Creek edges + emitter markers: dim line range. Confined to the pool in
    // Blind mode; visible across the map otherwise.
    glUniform1f(uLineFade_, gridFade);
    glUniform1f(uLineFlashAmount_, 0.0f);
    for (const DrawRange& r : ranges_) {
        glUniform3f(uLineColor_, r.color.x, r.color.y, r.color.z);
        glUniform1f(uLineBright_, fullBright ? 1.0f : blind ? 0.2f : 0.5f);
        glDrawArrays(GL_LINES, r.first, r.count);
    }

    // Solid objects (trees, boulders): additive filled shapes + localized bump
    // flash. In Blind mode they surface from black by proximity + reveal-on-
    // sound (revealRad + per-object reveal). In Lit/FullBright they are fully
    // lit everywhere (revealRad = 0 disables the fade, audio level forced to 1).
    {
        glUseProgram(objProgram_);
        glUniformMatrix4fv(uObjMVP_, 1, GL_FALSE_, glm::value_ptr(mvp));
        glUniform3f(uObjPlayer_, eyePos.x, eyePos.y, eyePos.z);
        glUniform1f(uObjReveal_, revealRad);
        glUniform1f(uObjFlashRadius_, 1.1f);
        glBindVertexArray(objVAO_);
        for (const DrawRange& r : objRanges_) {
            float flash = 0.0f, reveal = 0.0f;
            glm::vec3 flashPos{0.0f};
            if (r.treeIndex >= 0) {
                const Tree& t = scene.trees[static_cast<size_t>(r.treeIndex)];
                flash = t.flash; flashPos = t.flashPos; reveal = t.reveal;
            } else if (r.boulderIndex >= 0) {
                const Boulder& b = scene.boulders[static_cast<size_t>(r.boulderIndex)];
                flash = b.flash; flashPos = b.flashPos; reveal = b.reveal;
            }
            // Outside Blind mode there is no proximity term, so a base audio
            // level keeps objects fully lit across the map.
            const float base = blind ? reveal : (fullBright ? 1.0f : 0.75f);
            glUniform3f(uObjColor_, r.color.x, r.color.y, r.color.z);
            glUniform1f(uObjAudio_, base);
            glUniform1f(uObjFlashAmount_, fullBright ? 0.0f : flash);
            glUniform3f(uObjFlashPos_, flashPos.x, flashPos.y, flashPos.z);
            glDrawArrays(GL_TRIANGLES, r.first, r.count);
        }
    }

    // Bugs: small solid diamonds that move every frame (unit diamond +
    // uOffset). In Blind mode reveal-on-sound drives their glow so they light
    // up in time with their drone; otherwise they are steadily visible. A faint
    // wing-beat shimmer rides on top. Line program's diamond (cheap).
    glUseProgram(lineProgram_);
    glBindVertexArray(lineVAO_);   // object pass bound objVAO_; the diamond
                                   // lives in the line VBO (pos3 layout).
    if (!scene.bugs.empty()) {
        glUniform1f(uLineFade_, 0.0f);
        glUniform1f(uLineFlashAmount_, 0.0f);
        glUniform3f(uLineColor_, 1.0f, 0.55f, 0.85f);
        for (size_t i = 0; i < scene.bugs.size(); ++i) {
            const glm::vec3& bp = scene.bugs[i].pos;
            const float shimmer = 0.06f + 0.04f * std::sin(
                timeSeconds * 9.0f + static_cast<float>(i) * 2.1f);
            float bright;
            if (fullBright) {
                bright = 1.0f;
            } else if (blind) {
                // Blind: a bug is only visible when it is near enough to reveal
                // (same radius as objects) OR is actively sounding. No constant
                // floor -- distant/silent bugs are fully dark. The shimmer only
                // rides on top of an already-visible bug.
                const float d = glm::length(bp - eyePos);
                float prox = std::clamp(1.0f - d / kRevealRadius, 0.0f, 1.0f);
                prox *= prox;
                const float vis = std::max(prox, scene.bugs[i].reveal);
                bright = vis > 0.01f ? vis * (0.85f + shimmer) : 0.0f;
            } else {
                bright = 0.6f + shimmer;   // Lit: steadily visible
            }
            glUniform1f(uLineBright_, bright);
            glUniform3f(uLineOffset_, bp.x, bp.y, bp.z);
            glDrawArrays(GL_LINES, previewFirst_[2], previewCount_[2]);
        }
        glUniform3f(uLineOffset_, 0.0f, 0.0f, 0.0f);
    }

    // Stalkers: a menacing red diamond, larger than a bug, with a slow ominous
    // pulse. Like bugs it only surfaces in Blind mode by proximity or when its
    // drone is sounding (reveal) -- so mostly you feel it by ear, and glimpse it
    // only when it is nearly on you.
    if (!scene.stalkers.empty()) {
        glUniform1f(uLineFade_, 0.0f);
        glUniform1f(uLineFlashAmount_, 0.0f);
        glUniform3f(uLineColor_, 1.0f, 0.15f, 0.1f);
        glUniform1f(uLineScale_, 2.2f);   // bigger silhouette than a bug
        for (const Stalker& st : scene.stalkers) {
            const float pulse = 0.10f + 0.06f * std::sin(timeSeconds * 2.2f);
            float bright;
            if (fullBright) {
                bright = 1.0f;
            } else if (blind) {
                const float d = glm::length(st.pos - eyePos);
                float prox = std::clamp(1.0f - d / kRevealRadius, 0.0f, 1.0f);
                prox *= prox;
                const float vis = std::max(prox, st.reveal);
                bright = vis > 0.01f ? (0.5f + vis) + pulse : 0.0f;
            } else {
                bright = 0.7f + pulse;   // Lit: steadily visible
            }
            glUniform1f(uLineBright_, bright);
            glUniform3f(uLineOffset_, st.pos.x, st.pos.y, st.pos.z);
            glDrawArrays(GL_LINES, previewFirst_[2], previewCount_[2]);
        }
        glUniform3f(uLineOffset_, 0.0f, 0.0f, 0.0f);
        glUniform1f(uLineScale_, 1.0f);
    }

    // Dev placement ghost / paint brush ring: pulsing wireframe at the aim
    // point, visible in every mode (it is a dev tool, not part of the
    // proprioceptive look).
    if (preview != nullptr && preview->shape >= 0 && preview->shape < 4) {
        const float pulse = 0.35f + 0.25f * std::sin(timeSeconds * 6.0f);
        const glm::vec3 colors[4] = {
            { 0.35f, 1.0f, 0.45f },   // tree green
            { 1.0f, 0.55f, 0.3f },    // boulder orange
            { 0.6f, 0.8f, 1.0f },     // emitter blue
            { 1.0f, 1.0f, 1.0f },     // brush ring (usually overridden)
        };
        const glm::vec3 c = preview->hasColor ? preview->color : colors[preview->shape];
        glUniform1f(uLineFade_, 0.0f);   // dev tool: visible regardless of pool
        glUniform1f(uLineFlashAmount_, 0.0f);
        glUniform3f(uLineColor_, c.x, c.y, c.z);
        glUniform1f(uLineBright_, pulse);
        glUniform3f(uLineOffset_, preview->pos.x, preview->pos.y, preview->pos.z);
        glUniform1f(uLineScale_, preview->shape == 3 ? preview->scale : 1.0f);
        glDrawArrays(GL_LINES, previewFirst_[preview->shape], previewCount_[preview->shape]);
        glUniform3f(uLineOffset_, 0.0f, 0.0f, 0.0f);
        glUniform1f(uLineScale_, 1.0f);
    }

    // Glow pass
    glUseProgram(glowProgram_);
    glUniformMatrix4fv(uGlowMVP_, 1, GL_FALSE_, glm::value_ptr(mvp));
    glUniform3f(uGlowPlayer_, eyePos.x, eyePos.y, 0.0f);

    // Painted floor cells: one tinted + patterned pass per material, so zones
    // read by both color and texture. Confined to the lit pool in Blind mode
    // (you only see the ground you are on); across the whole map otherwise.
    // Brighter outside Blind so the zones are obvious from a distance.
    if (!floorRanges_.empty()) {
        glBindVertexArray(floorVAO_);
        glUniform3f(uGlowOffset_, 0.0f, 0.0f, 0.0f);
        glUniform1f(uGlowFade_, floorFade);
        for (const FloorRange& r : floorRanges_) {
            const glm::vec3 c = floorColor(r.material);
            glUniform3f(uGlowColor_, c.x, c.y, c.z);
            glUniform1i(uGlowPattern_, blind ? 0 : floorPattern(r.material));
            glUniform1f(uGlowIntensity_, fullBright ? 0.30f : blind ? 0.18f : 0.32f);
            glDrawArrays(GL_TRIANGLES, r.first, r.count);
        }
    }
    glUniform1i(uGlowPattern_, 0);   // rest of the glow pass is solid

    // Zone boundary lines: bright segments tracing material transitions, so the
    // edges between zones are crisp and unmistakable. Skipped in Blind mode
    // (there you are meant to feel zones by ear/footstep, not read the map).
    if (!blind && floorEdgeCount_ > 0) {
        glUseProgram(lineProgram_);
        glBindVertexArray(floorEdgeVAO_);
        glUniform1f(uLineFade_, 0.0f);
        glUniform1f(uLineFlashAmount_, 0.0f);
        glUniform3f(uLineOffset_, 0.0f, 0.0f, 0.0f);
        glUniform1f(uLineScale_, 1.0f);
        glUniform3f(uLineColor_, 0.95f, 0.97f, 1.0f);   // near-white edges
        glUniform1f(uLineBright_, fullBright ? 0.9f : 0.55f);
        glDrawArrays(GL_LINES, 0, floorEdgeCount_);
        glUseProgram(glowProgram_);
    }

    glBindVertexArray(glowVAO_);

    // Creek band: faint slow pulse. Confined to the pool in Blind mode.
    {
        const float pulse = 0.05f + 0.03f * (0.5f + 0.5f * std::sin(timeSeconds * 0.9f));
        glUniform3f(uGlowOffset_, 0.0f, 0.0f, 0.0f);
        glUniform1f(uGlowFade_, floorFade);
        glUniform3f(uGlowColor_, 0.3f, 0.6f, 1.0f);
        glUniform1f(uGlowIntensity_, fullBright ? 0.15f : pulse);
        glDrawArrays(GL_TRIANGLES, creekFirst_, creekCount_);
    }

    // Ground glow disc under the player (brightened briefly by footsteps). This
    // is the core of the lit pool -- always drawn, never faded (it is centered
    // on the player), so the feet are always lit.
    glUniform3f(uGlowOffset_, eyePos.x, eyePos.y, 0.0f);
    glUniform1f(uGlowFade_, 0.0f);
    glUniform3f(uGlowColor_, 0.55f, 0.65f, 0.8f);
    glUniform1f(uGlowIntensity_, 0.12f + 0.25f * groundGlowBoost);
    glDrawArrays(GL_TRIANGLES, discFirst_, discCount_);

    glBindVertexArray(0);
    glDisable(GL_BLEND);
}
