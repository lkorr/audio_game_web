#pragma once
// F2 dev tools with an on-screen overlay UI (5x7 pixel font, see Renderer).
// Three states:
//
//   INSPECT (default)  a view ray picks the object under the crosshair every
//                      frame; the pick is highlighted (surface flash / pulsing
//                      diamond) and named in the overlay. With nothing picked
//                      the floor material at the aim point is the target.
//                        E / Enter / LMB  edit target (opens menu)
//                        X delete target | B place mode | T paint mode
//                        O worlds (save/load browser) | L reload active file
//                        V fly | U rescan | C save scene | P print pos | F2 exit
//
//   PLACE              the old placement ghost flow.
//                        1-4 type | LMB/Enter place | wheel or R/F distance
//                        Q/E height | [ ] emitter sound | ESC/B back
//
//   PAINT              floor paving. A brush ring sits at the ground aim
//                      point; holding LMB paints the selected material.
//                        LMB/Enter paint | wheel or R/F brush radius
//                        [ ] material | K rect corner / commit (pave by area)
//                        X eyedrop | N new material | E edit material
//                      The overlay shows per-material coverage in m^2.
//
//   MENU               edit panel for the chosen object or floor material.
//                        Up/Down row | Left/Right (or wheel) adjust
//                        Enter audition / toggle | ESC/E close
//                      Object menus have two pages on the PAGE row: SOUNDS
//                      (one SoundSlot at a time, the SLOT row switches) and
//                      BODY (position + dimensions / behavior stats). All
//                      edits apply live (SoundDirector diffs loop configs;
//                      body edits re-upload geometry).
//
//   ENGINE (G)         the VST dev panel: live xyzpan engine tuning, grouped
//                      into categories (left/right, front/behind, above/below,
//                      body/floor, distance, reverb, smoothing, bypasses).
//                      Category list -> param list; same navigation as MENU,
//                      X resets a param to default. Values apply to every
//                      voice next frame; non-defaults persist in scene.txt.
//
//   WORLDS (O)         save/load browser over scene.txt + worlds/*.txt.
//                        Up/Down select | Enter load | S save over selected
//                        N save to a new slot | ESC/O back
//
// Console output remains as a log; the overlay is the source of truth.

#include "Scene.h"
#include "AudioWorld.h"
#include "EngineTuning.h"
#include "Player.h"
#include "Renderer.h"
#include <cstdint>
#include <string>
#include <vector>

class SoundLibrary;
class SoundDirector;

class DevMode {
public:
    bool enabled = false;

    void init(SoundLibrary* lib, SoundDirector* director, EngineTuning* tuning,
              std::string scenePath);

    void toggle(Player& player);

    // Once per frame while enabled: refresh the inspect pick and apply the
    // highlight flash to the targeted object. In PAINT, a held left button
    // drag-paints under the brush (hence the Renderer for floor re-uploads).
    void frameUpdate(Scene& scene, const Player& player, Renderer& renderer);

    // Input. handleKey returns true if the key was consumed (main then skips
    // its own bindings -- ESC in menu/place closes instead of quitting).
    // Repeats are passed through so held arrows keep adjusting.
    bool handleKey(int key, bool repeat, Scene& scene, Player& player,
                   const ListenerPose& listener, Renderer& renderer);
    void handleWheel(float dy, Scene& scene, const ListenerPose& listener,
                     Renderer& renderer);
    void handleClick(Scene& scene, const Player& player, Renderer& renderer);

    // Placement ghost (PLACE) or emitter highlight diamond (INSPECT/MENU).
    bool preview(const Player& player, DevPreview& out) const;

    // Screen-space UI for the current state.
    void buildOverlay(const Scene& scene, int fbWidth, int fbHeight,
                      std::vector<OverlayRect>& rects,
                      std::vector<OverlayText>& texts) const;

private:
    enum class State { Inspect, Place, Paint, Menu, EngineMenu, Worlds };
    enum class TargetKind { None, Tree, Boulder, Emitter, Bug, Floor };
    enum class PlaceType : int { Tree = 0, Boulder, Emitter, Bug, kCount };
    enum class MenuPage { Sounds, Body };

    // One editable line in the menu. PosX..SizeD are BODY-page rows whose
    // meaning depends on the target kind (labels come from rowLabel).
    enum class Row {
        Page, Slot, Sound, Enabled, Gain, AudibleRadius, Verb, Doppler, DopplerScale,
        MinInterval, MaxInterval, Radius, Cooldown,
        PitchJit, GainJit, Audition,
        PosX, PosY, PosZ, SizeA, SizeB, SizeC, SizeD,
    };

    struct Target {
        TargetKind kind = TargetKind::None;
        uint32_t id = 0;          // object target
        int material = 0;         // floor target (GroundMaterial index)
        glm::vec3 hitPos{0.0f};   // ray hit, drives the highlight flash
        float rayT = 0.0f;
    };

    SoundLibrary* lib_ = nullptr;
    SoundDirector* director_ = nullptr;
    EngineTuning* tuning_ = nullptr;
    std::string scenePath_ = "scene.txt";

    State state_ = State::Inspect;
    Target target_;               // refreshed per frame in INSPECT
    Target menuTarget_;           // frozen while MENU is open

    // PLACE state
    PlaceType type_ = PlaceType::Tree;
    float dist_ = 4.0f;
    float heightOffset_ = 0.0f;
    int placeSound_ = -1;

    // PAINT state
    int paintMat_ = 1;            // selected floor material
    float brushRadius_ = 2.0f;
    bool hasRectCorner_ = false;  // first corner of a pending rect pave
    glm::vec2 rectCorner_{0.0f};

    // MENU state
    SoundSlot slot_ = SoundSlot::Loop;
    MenuPage page_ = MenuPage::Sounds;
    int row_ = 0;
    bool menuFromPaint_ = false;  // reopened from PAINT: close back into it

    // ENGINE state: -1 = category list, else open category index.
    int engCat_ = -1;
    int engRow_ = 0;

    // WORLDS state
    std::vector<std::string> worldFiles_;  // full paths; [0] = scenePath_
    int worldRow_ = 0;
    std::string worldMsg_;

    glm::vec3 aimPoint(const Player& player) const;
    // Ray-ground intersection of the view ray (false when looking level/up).
    bool aimGround(const Player& player, glm::vec2& out) const;
    Target pick(const Scene& scene, const Player& player) const;

    void openMenu(Scene& scene);
    void closeMenu();
    std::vector<Row> menuRows(const Scene& scene) const;
    std::string rowLabel(Row r) const;
    std::string rowValue(const Scene& scene, Row r) const;
    void adjustRow(Scene& scene, Row r, int dir, const ListenerPose& listener,
                   Renderer& renderer);

    // Resolve menuTarget_ to live scene data (nullptr if deleted).
    SoundProfile* menuProfile(Scene& scene, glm::vec3* posOut = nullptr) const;
    FloorMaterial* menuFloor(Scene& scene) const;
    Tree* menuTree(Scene& scene) const;
    Boulder* menuBoulder(Scene& scene) const;
    Emitter* menuEmitter(Scene& scene) const;
    Bug* menuBug(Scene& scene) const;

    void place(Scene& scene, const Player& player, Renderer& renderer);
    void deleteTarget(Scene& scene, Renderer& renderer);
    void auditionCurrent(Scene& scene, const ListenerPose& listener);

    bool handlePaintKey(int key, Scene& scene, const Player& player,
                        const ListenerPose& listener, Renderer& renderer);
    void paintBrush(Scene& scene, const Player& player, Renderer& renderer);

    bool handleWorldsKey(int key, Scene& scene, Renderer& renderer);
    void refreshWorldList();
    std::string worldsDir() const;
    void loadWorld(const std::string& path, Scene& scene, Renderer& renderer);
    void saveWorld(const std::string& path, const Scene& scene);

    bool handleEngineKey(int key);
    std::vector<int> engineCategoryParams(int category) const;

    const char* targetName(const Scene& scene, const Target& t, char* buf, size_t n) const;
};
