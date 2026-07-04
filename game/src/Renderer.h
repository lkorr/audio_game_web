#pragma once
// Dark proprioceptive wireframe renderer. GL 3.3 core, line primitives only
// (WebGL2/GLES3-compatible subset: no glPolygonMode, no geometry shaders).
// Additive blending, no depth buffer (additive lines commute).

#include "Scene.h"
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <string>
#include <vector>

// Visual modes:
//   Lit        — solid objects + floor lit across the whole map (default)
//   Blind      — "blind mode": near-total darkness; only a small pool of ground
//                around the player is lit and objects surface from black by
//                proximity + reveal-on-sound. The audio-first immersive look.
//   FullBright — everything at full brightness (debug, Tab)
enum class VisualMode { Lit, Blind, FullBright };

// Dev-mode placement ghost / inspect highlight: a pulsing wireframe drawn at
// `pos`. shape: 0 = tree, 1 = boulder, 2 = emitter diamond, 3 = ground circle
// (paint brush ring; `scale` is its radius in meters).
struct DevPreview {
    int shape = 0;
    glm::vec3 pos{0.0f};
    float scale = 1.0f;
    glm::vec3 color{0.0f};   // used when hasColor (paint brush tint)
    bool hasColor = false;
};

// Screen-space overlay primitives (dev UI). Coordinates in pixels from the
// top-left; drawn with standard alpha blending after the world (rects first,
// then text), so dark panels dim the scene behind them.
struct OverlayRect {
    float x = 0, y = 0, w = 0, h = 0;
    glm::vec4 color{0, 0, 0, 0.6f};
};

struct OverlayText {
    float x = 0, y = 0;
    float scale = 2.0f;              // pixel size of one font texel
    glm::vec4 color{1, 1, 1, 1};
    std::string text;                // 5x7 font; lowercase maps to uppercase
};

class Renderer {
public:
    bool init(const Scene& scene);
    void shutdown();

    // Re-upload the scene line geometry after dev-mode placement/deletion.
    // Also refreshes the painted-floor mesh.
    void rebuildScene(const Scene& scene);

    // Re-upload only the painted-floor mesh (dev paint mode, cheap).
    void rebuildFloor(const Scene& scene);

    // Display tint for a floor material index (paint overlay + floor mesh).
    static glm::vec3 floorColor(int mat);

    // flashes decay on the game thread (Scene::trees/boulders .flash fields).
    // preview: optional dev-mode placement ghost (nullptr = none).
    void draw(const Scene& scene, const glm::vec3& eyePos, float yaw, float pitch,
              int fbWidth, int fbHeight, float timeSeconds,
              float groundGlowBoost, VisualMode mode,
              const DevPreview* preview = nullptr);

    // Screen-space dev UI, call after draw(). Vertex data is rebuilt and
    // streamed every call -- fine at overlay scale.
    void drawOverlay(const std::vector<OverlayRect>& rects,
                     const std::vector<OverlayText>& texts,
                     int fbWidth, int fbHeight);

    // Pixel metrics of the 5x7 font (cell advance includes 1px spacing).
    static float textWidth(const std::string& s, float scale) {
        return static_cast<float>(s.size()) * 6.0f * scale;
    }
    static float textHeight(float scale) { return 8.0f * scale; }

private:
    struct DrawRange {
        int first = 0, count = 0;
        glm::vec3 color{1.0f};
        int treeIndex = -1;     // index into scene.trees for flash/reveal lookup
        int boulderIndex = -1;  // index into scene.boulders
    };

    // (Re)build and upload the world-space line list (creek edges + markers +
    // grid + preview unit shapes). Called by init() and rebuildScene().
    void uploadSceneGeometry(const Scene& scene);
    // (Re)build the solid tree/boulder triangle mesh (kObjVS/kObjFS program).
    void uploadObjectGeometry(const Scene& scene);

    unsigned lineProgram_ = 0, glowProgram_ = 0, objProgram_ = 0;
    // line VAO/VBO: world-space line list (creek edges + markers + grid)
    unsigned lineVAO_ = 0, lineVBO_ = 0;
    std::vector<DrawRange> ranges_;
    // object VAO/VBO: solid tree/boulder triangles (pos3 + shade1)
    unsigned objVAO_ = 0, objVBO_ = 0;
    std::vector<DrawRange> objRanges_;
    // grid: separate range with distance fade
    int gridFirst_ = 0, gridCount_ = 0;
    // dev preview unit shapes at origin (tree / boulder / diamond / unit
    // circle), drawn with uOffset (+ uScale for the circle)
    int previewFirst_[4] = {}, previewCount_[4] = {};
    // overlay VAO/VBO: screen-space quads (pos2 + color4), streamed per frame
    unsigned overlayProgram_ = 0, overlayVAO_ = 0, overlayVBO_ = 0;
    int uOverlayScreen_ = -1;
    std::vector<float> overlayVerts_;  // scratch, reused across frames
    // glow VAO/VBO: player disc + creek band (triangles with per-vertex alpha)
    unsigned glowVAO_ = 0, glowVBO_ = 0;
    int discFirst_ = 0, discCount_ = 0;
    int creekFirst_ = 0, creekCount_ = 0;
    // floor VAO/VBO: painted ground cells (triangles, glow shader), one draw
    // range per non-default material
    struct FloorRange { int first = 0, count = 0, material = 0; };
    unsigned floorVAO_ = 0, floorVBO_ = 0;
    std::vector<FloorRange> floorRanges_;
    // floor boundary lines: segments where adjacent cells have different
    // materials (drawn bright so zone edges are crisp). Line program, pos3.
    unsigned floorEdgeVAO_ = 0, floorEdgeVBO_ = 0;
    int floorEdgeCount_ = 0;

    void uploadFloorGeometry(const Scene& scene);

    // uniform locations
    int uLineMVP_ = -1, uLineColor_ = -1, uLineBright_ = -1, uLinePlayer_ = -1, uLineFade_ = -1;
    int uLineFlashPos_ = -1, uLineFlashAmount_ = -1, uLineFlashRadius_ = -1;
    int uLineOffset_ = -1, uLineScale_ = -1;
    int uObjMVP_ = -1, uObjColor_ = -1, uObjPlayer_ = -1, uObjReveal_ = -1, uObjAudio_ = -1;
    int uObjFlashPos_ = -1, uObjFlashAmount_ = -1, uObjFlashRadius_ = -1;
    int uGlowMVP_ = -1, uGlowOffset_ = -1, uGlowColor_ = -1, uGlowIntensity_ = -1;
    int uGlowPlayer_ = -1, uGlowFade_ = -1, uGlowPattern_ = -1;

    // Display pattern id for a floor material (matches kGlowFS uPattern).
    static int floorPattern(int mat);
};
