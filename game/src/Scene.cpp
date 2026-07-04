#include "Scene.h"
#include "SoundLibrary.h"
#include "Stalker.h"
#include <algorithm>
#include <cmath>

const char* soundSlotName(SoundSlot s) {
    switch (s) {
        case SoundSlot::Loop:       return "loop";
        case SoundSlot::IdleRandom: return "idle";
        case SoundSlot::Proximity:  return "near";
        case SoundSlot::Impact:     return "impact";
        default:                    return "?";
    }
}

namespace {

SoundRule loopRule(int set, float gain, float verbWet, bool doppler) {
    SoundRule r;
    r.soundSet = set;
    r.gain = gain;
    r.verbWet = verbWet;
    r.doppler = doppler;
    return r;
}

SoundRule impactRule(int set, float gain) {
    SoundRule r;
    r.soundSet = set;
    r.gain = gain;
    r.verbWet = 0.0f;
    r.doppler = false;   // impacts happen at arm's length
    r.cooldown = 0.5f;
    return r;
}

} // namespace

void Scene::paintCircle(float x, float y, float radius, int mat) {
    if (mat < 0 || mat >= static_cast<int>(materials.size())) return;
    const int i0 = cellIndexOf(x - radius), i1 = cellIndexOf(x + radius);
    const int j0 = cellIndexOf(y - radius), j1 = cellIndexOf(y + radius);
    const float r2 = radius * radius;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const float cx = -kHalfExtent + (static_cast<float>(i) + 0.5f) * kCellSize;
            const float cy = -kHalfExtent + (static_cast<float>(j) + 0.5f) * kCellSize;
            const float dx = cx - x, dy = cy - y;
            if (dx * dx + dy * dy <= r2) cell(i, j) = static_cast<uint8_t>(mat);
        }
}

void Scene::paintRect(glm::vec2 a, glm::vec2 b, int mat) {
    if (mat < 0 || mat >= static_cast<int>(materials.size())) return;
    const int i0 = cellIndexOf(std::min(a.x, b.x)), i1 = cellIndexOf(std::max(a.x, b.x));
    const int j0 = cellIndexOf(std::min(a.y, b.y)), j1 = cellIndexOf(std::max(a.y, b.y));
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            cell(i, j) = static_cast<uint8_t>(mat);
}

float Scene::areaOf(int mat) const {
    size_t n = 0;
    for (uint8_t c : floor)
        if (c == mat) ++n;
    return static_cast<float>(n) * kCellArea;
}

SoundRule makeBugLoopRule(const SoundLibrary& lib) {
    SoundRule r;
    r.soundSet = lib.find("saw");
    r.gain = 0.45f;
    r.audibleRadius = 6.0f;   // tiny: audible only up close
    r.verbWet = 0.0f;
    r.doppler = true;
    r.dopplerScale = 3.0f;    // exaggerated pitch swing on flybys
    return r;
}

// Multi-sine wander: incommensurate frequencies give a non-repeating-looking
// path; per-bug phase from the id (golden-angle spread) decorrelates bugs.
// Peak horizontal speed ~= wanderRadius * 1.06 m/s.
void updateBugs(Scene& scene, float dt) {
    const float lim = Scene::kHalfExtent - 0.5f;
    for (Bug& b : scene.bugs) {
        b.t += dt;
        const float ph = static_cast<float>(b.id) * 2.39996f;
        const float t = b.t + ph * 7.0f;
        const float R = b.wanderRadius;
        b.pos.x = b.home.x + R * (0.6f * std::sin(0.70f * t + ph)
                                + 0.4f * std::sin(1.60f * t + 2.0f * ph));
        b.pos.y = b.home.y + R * (0.6f * std::sin(0.62f * t + 1.7f * ph)
                                + 0.4f * std::sin(1.45f * t + 0.8f * ph));
        b.pos.z = std::max(0.4f, b.home.z + 0.30f * std::sin(0.9f * t + ph)
                                         + 0.15f * std::sin(2.3f * t));
        b.pos.x = std::clamp(b.pos.x, -lim, lim);
        b.pos.y = std::clamp(b.pos.y, -lim, lim);
    }
}

Scene buildScene(const SoundLibrary& lib) {
    Scene s;

    const int water = lib.find("water");
    const int wind = lib.find("wind");
    const int bird = lib.find("bird");
    const int knock = lib.find("knock");
    const int thud = lib.find("thud");

    // Footstep materials (floor grid indices). A material per zone: its timbre
    // is how you tell zones apart by ear. Palette colors (Renderer::floorColor)
    // line up by index: 0 grass, 1 stone, 2 gold/wood, 3 purple.
    s.materials.push_back({ "grass", lib.find("grass"), 0.7f, 0.05f, 2.0f });
    s.materials.push_back({ "stone", lib.find("stone"), 0.7f, 0.05f, 2.0f });
    s.materials.push_back({ "wood",  lib.find("stone"), 0.6f, 0.08f, 2.0f });  // grove boardwalk
    s.materials.push_back({ "cave",  lib.find("stone"), 0.8f, 0.03f, 2.0f });  // cavern floor
    const int stone = 1, wood = 2, cave = 3;

    // ---- Acoustic zones ----
    // The world is 90x90 m. Around the central glade sit distinct rooms, each
    // with its own reverb signature (verbWet on its emitters), floor material
    // (footstep timbre), and boulder "walls" that occlude sound between rooms.
    // Zones are just data -- no zone subsystem; see Scene.h header note.

    auto addTree = [&](glm::vec2 p) {
        Tree t; t.pos = p; t.id = s.takeId();
        t.sounds.rule(SoundSlot::Impact) = impactRule(knock, 0.55f);
        s.trees.push_back(t);
    };
    auto addBoulder = [&](glm::vec3 c, glm::vec3 half) {
        Boulder b; b.center = c; b.half = half; b.id = s.takeId();
        b.sounds.rule(SoundSlot::Impact) = impactRule(thud, 0.6f);
        s.boulders.push_back(b);
    };
    auto addEmitter = [&](glm::vec3 pos, const SoundRule& loop) {
        Emitter e; e.pos = pos; e.id = s.takeId();
        e.sounds.rule(SoundSlot::Loop) = loop;
        s.emitters.push_back(std::move(e));
    };
    auto addBug = [&](glm::vec3 home) {
        Bug b; b.home = home; b.pos = home; b.id = s.takeId();
        b.sounds.rule(SoundSlot::Loop) = makeBugLoopRule(lib);
        s.bugs.push_back(b);
    };
    // A straight wall of boulders with a gap, to separate rooms acoustically.
    auto addWall = [&](glm::vec2 a, glm::vec2 b, float gapCenter, float gapHalf) {
        const glm::vec2 d = b - a;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const glm::vec2 dir = d / std::max(len, 0.001f);
        for (float t = 0.0f; t <= len; t += 1.6f) {
            if (std::abs(t - gapCenter) < gapHalf) continue;   // doorway
            const glm::vec2 p = a + dir * t;
            addBoulder({ p.x, p.y, 0.8f }, { 0.9f, 0.9f, 0.8f });
        }
    };

    // ---- Central glade (the original scene; dry, grassy, the creek) ----
    // Stone band along the creek. Tree 0 stays just right of the render-test
    // walk path (x = 0, y: -7 -> 8), so the pass goes left of its bird emitter.
    s.paintRect({ -Scene::kHalfExtent, Scene::kCreekY - Scene::kStoneHalfWidth },
                { Scene::kHalfExtent, Scene::kCreekY + Scene::kStoneHalfWidth }, stone);
    addTree({ 1.5f, -2.0f });
    addTree({ -6.0f, 3.0f });
    addTree({ 8.0f, -5.0f });
    addTree({ -12.0f, -8.0f });
    addTree({ 10.0f, 9.5f });
    addTree({ -4.0f, 12.0f });
    addTree({ 13.0f, -12.0f });
    addTree({ -15.0f, 6.0f });
    addBoulder({ 3.0f, -10.0f, 0.5f }, { 0.6f, 0.5f, 0.5f });
    addBoulder({ -9.0f, -3.0f, 0.4f }, { 0.5f, 0.6f, 0.4f });
    addBoulder({ 9.0f, 3.0f, 0.45f }, { 0.55f, 0.45f, 0.45f });
    addEmitter({ -7.0f, Scene::kCreekY, 0.1f }, loopRule(water, 0.9f, 0.05f, true));
    addEmitter({ 6.0f, Scene::kCreekY, 0.1f }, loopRule(water, 0.9f, 0.05f, true));
    addEmitter({ 1.5f, -2.0f, 3.0f }, loopRule(bird, 0.8f, 0.08f, true));
    addEmitter({ 10.0f, 9.5f, 3.0f }, loopRule(bird, 0.8f, 0.08f, true));
    addEmitter({ -4.0f, 12.0f, 3.0f }, loopRule(bird, 0.8f, 0.08f, true));
    addEmitter({ 0.0f, 0.0f, 10.0f }, loopRule(wind, 0.22f, 0.0f, false));
    addBug({ -3.0f, -4.0f, 1.6f });
    addBug({ 5.0f, 2.0f, 1.5f });

    // ---- Cavern (northeast, x:24..42 y:22..40): wet, stone, boulder walls.
    // Water drips + wind resonate with heavy reverb -- the acoustic opposite of
    // the open glade. A boulder wall with a doorway on the west side gates it.
    s.paintRect({ 22.0f, 20.0f }, { 42.0f, 42.0f }, cave);
    addWall({ 22.0f, 20.0f }, { 22.0f, 42.0f }, 8.0f, 2.2f);   // west wall + door
    addWall({ 22.0f, 20.0f }, { 42.0f, 20.0f }, 100.0f, 0.0f); // south wall (solid)
    addEmitter({ 33.0f, 31.0f, 1.0f }, loopRule(water, 0.8f, 0.6f, true));
    addEmitter({ 27.0f, 37.0f, 2.0f }, loopRule(wind, 0.3f, 0.55f, false));
    addBug({ 30.0f, 28.0f, 1.4f });
    addBug({ 37.0f, 34.0f, 1.7f });

    // ---- Dense grove (southwest, x:-42..-22 y:-40..-18): dry, wooden floor,
    // many trees packed close (natural occlusion) with birdsong overhead.
    s.paintRect({ -42.0f, -40.0f }, { -22.0f, -18.0f }, wood);
    for (int i = 0; i < 10; ++i) {
        const float gx = -40.0f + static_cast<float>((i * 7) % 18);
        const float gy = -38.0f + static_cast<float>((i * 11) % 20);
        addTree({ gx, gy });
        if (i % 3 == 0)
            addEmitter({ gx, gy, 3.0f }, loopRule(bird, 0.7f, 0.06f, true));
    }

    // ---- Far ridge (north edge, y ~ 40): a lone high wind, very low, that you
    // can only just hear -- a distant landmark to orient by across the big map.
    addEmitter({ -30.0f, 40.0f, 12.0f }, loopRule(wind, 0.18f, 0.15f, false));

    return s;
}

Scene buildSliceScene(const SoundLibrary& lib) {
    Scene s;

    const int music = lib.find("music");
    const int water = lib.find("water");
    const int wind = lib.find("wind");
    const int bird = lib.find("bird");
    const int thud = lib.find("thud");
    const int knock = lib.find("knock");

    // Four zone materials, each a footstep timbre + palette color the player
    // learns by ear (grass green / stone blue-grey / wood gold / cave purple;
    // Renderer::floorColor lines up by index). Grass reads soft to the Stalker,
    // stone rings loud -- the floor is a stealth choice you make per step.
    s.materials.push_back({ "grass", lib.find("grass"), 0.55f, 0.05f, 2.0f });
    s.materials.push_back({ "stone", lib.find("stone"), 0.9f,  0.05f, 2.0f });  // creek + hazard
    s.materials.push_back({ "wood",  lib.find("stone"), 0.6f,  0.08f, 2.0f });  // grove floor (quieter)
    s.materials.push_back({ "cave",  lib.find("stone"), 0.85f, 0.03f, 2.0f });  // cavern floor (loud, ringing)
    const int stone = 1, wood = 2, cave = 3;

    // ---- Local builders (the buildScene pattern, copied in) ----
    auto addBoulder = [&](glm::vec3 c, glm::vec3 half) {
        Boulder b; b.center = c; b.half = half; b.id = s.takeId();
        b.sounds.rule(SoundSlot::Impact) = impactRule(thud, 0.6f);
        s.boulders.push_back(b);
    };
    auto addTree = [&](glm::vec2 p) {
        Tree t; t.pos = p; t.id = s.takeId();
        t.sounds.rule(SoundSlot::Impact) = impactRule(knock, 0.55f);
        s.trees.push_back(t);
    };
    auto addEmitter = [&](glm::vec3 pos, const SoundRule& loop) {
        Emitter e; e.pos = pos; e.id = s.takeId();
        e.sounds.rule(SoundSlot::Loop) = loop;
        s.emitters.push_back(std::move(e));
    };
    auto addBug = [&](glm::vec3 home) {
        Bug b; b.home = home; b.pos = home; b.id = s.takeId();
        b.sounds.rule(SoundSlot::Loop) = makeBugLoopRule(lib);
        s.bugs.push_back(b);
    };
    // A run of boulders from a->b with an optional doorway gap: a rough wall you
    // route around, and which breaks the Stalker's line of sound.
    auto addWall = [&](glm::vec2 a, glm::vec2 b, float gapCenter = 1e9f,
                       float gapHalf = 0.0f) {
        const glm::vec2 d = b - a;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        const glm::vec2 dir = d / std::max(len, 0.001f);
        for (float t = 0.0f; t <= len; t += 1.7f) {
            if (std::abs(t - gapCenter) < gapHalf) continue;   // doorway
            const glm::vec2 p = a + dir * t;
            addBoulder({ p.x, p.y, 0.9f }, { 1.0f, 1.0f, 0.9f });
        }
    };
    // A sanctuary: a music emitter whose loop repels the Stalker within radius,
    // and which the player hears in the dark as a safe pocket / checkpoint.
    auto addSanctuary = [&](glm::vec3 pos, float radius) {
        Emitter e; e.pos = pos; e.id = s.takeId();
        SoundRule r = loopRule(music, 0.6f, 0.2f, false);
        r.audibleRadius = radius + 8.0f;   // heard a little beyond its safe edge
        e.sounds.rule(SoundSlot::Loop) = r;
        e.isMusic = true;
        e.musicRadius = radius;
        s.emitters.push_back(std::move(e));
    };

    // =====================================================================
    // ONE living glade. The river (creek stone band at y = kCreekY) flows
    // E->W across the middle as the master landmark you orient by. You start
    // in a riverside sanctuary at the south, then route north through/around
    // the river toward its far source (the goal), hopping between three more
    // sanctuaries tucked in a wet cavern (NE) and a wooden grove (W). One
    // Stalker owns the river crossing -- the chokepoint between south and
    // north. Trees, boulder walls, and the river itself are your cover.
    // =====================================================================

    // ---- The river: creek stone band across the map, strung with water. ----
    // A clear, continuous audible line the whole world is arranged around.
    s.paintRect({ -Scene::kHalfExtent, Scene::kCreekY - Scene::kStoneHalfWidth },
                { Scene::kHalfExtent, Scene::kCreekY + Scene::kStoneHalfWidth }, stone);
    addEmitter({ -22.0f, Scene::kCreekY, 0.1f }, loopRule(water, 0.85f, 0.05f, true));
    addEmitter({ -8.0f,  Scene::kCreekY, 0.1f }, loopRule(water, 0.9f,  0.05f, true));
    addEmitter({ 8.0f,   Scene::kCreekY, 0.1f }, loopRule(water, 0.9f,  0.05f, true));
    addEmitter({ 22.0f,  Scene::kCreekY, 0.1f }, loopRule(water, 0.85f, 0.05f, true));

    // ---- South glade (start): grass, trees, birds, bugs, a low wind. ----
    // Open and calm -- the safe home you set out from and can retreat toward.
    addTree({ 3.0f, -30.0f });  addTree({ -5.0f, -26.0f });
    addTree({ 7.0f, -20.0f });  addTree({ -9.0f, -18.0f });
    addTree({ 1.0f, -12.0f });
    addEmitter({ 3.0f, -30.0f, 3.0f }, loopRule(bird, 0.75f, 0.06f, true));
    addEmitter({ -9.0f, -18.0f, 3.0f }, loopRule(bird, 0.7f, 0.06f, true));
    addEmitter({ 0.0f, -28.0f, 10.0f }, loopRule(wind, 0.20f, 0.0f, false));
    addBug({ -4.0f, -24.0f, 1.6f });
    addBug({ 6.0f, -16.0f, 1.5f });

    // Start sanctuary + respawn: a riverside pocket at the south.
    addSanctuary({ 0.0f, -30.0f, 1.2f }, 7.0f);

    // ---- The river crossing (the Stalker's chokepoint). ----
    // Boulder groins reach in from both banks, leaving a doorway you must pass
    // through to get north -- the tightest, most dangerous point on the route.
    // Skirt the far bank quietly (grass) or break line-of-sound behind the
    // boulders while the Stalker patrols the crossing.
    addWall({ -18.0f, 2.0f }, { -6.0f, 2.0f });     // south groin, west
    addWall({ 6.0f, 2.0f }, { 18.0f, 2.0f });       // south groin, east
    addWall({ -14.0f, 14.0f }, { -2.0f, 14.0f });   // north groin, west
    addWall({ 4.0f, 14.0f }, { 16.0f, 14.0f });     // north groin, east
    // A stone apron on the south bank: loud footing right at the crossing that
    // rings out and draws the Stalker -- the careful player skirts it on grass.
    s.paintRect({ -5.0f, -1.0f }, { 5.0f, 1.0f }, stone);
    addTree({ -3.0f, 4.0f });  addTree({ 4.0f, 5.0f });   // cover at the crossing

    // ---- Wooden grove (west, x:-42..-24 y:-14..12): dense trees, soft floor.
    // Packed trunks are natural occlusion; a quieter wooden floor rewards a
    // detour west to break the Stalker's trail. A sanctuary sits at its heart.
    s.paintRect({ -42.0f, -14.0f }, { -24.0f, 12.0f }, wood);
    for (int i = 0; i < 11; ++i) {
        const float gx = -40.0f + static_cast<float>((i * 5) % 15);
        const float gy = -12.0f + static_cast<float>((i * 9) % 22);
        addTree({ gx, gy });
        if (i % 4 == 0)
            addEmitter({ gx, gy, 3.0f }, loopRule(bird, 0.65f, 0.06f, true));
    }
    addBug({ -33.0f, -2.0f, 1.5f });
    addSanctuary({ -32.0f, -2.0f, 1.2f }, 6.5f);   // grove checkpoint

    // ---- Wet cavern (northeast, x:16..40 y:20..40): loud ringing floor, heavy
    // reverb, boulder walls with a west doorway. The acoustic opposite of the
    // open grove -- dripping water + a resonant wind. A sanctuary hides deep in
    // it, a tense safe room you reach through the door.
    s.paintRect({ 16.0f, 20.0f }, { 40.0f, 40.0f }, cave);
    addWall({ 16.0f, 20.0f }, { 16.0f, 40.0f }, 6.0f, 2.4f);   // west wall + door
    addWall({ 16.0f, 20.0f }, { 40.0f, 20.0f });               // south wall (solid)
    addEmitter({ 30.0f, 30.0f, 1.0f }, loopRule(water, 0.75f, 0.6f, true));
    addEmitter({ 22.0f, 36.0f, 2.0f }, loopRule(wind, 0.28f, 0.55f, false));
    addBug({ 27.0f, 27.0f, 1.4f });
    addSanctuary({ 33.0f, 34.0f, 1.2f }, 6.0f);    // cavern checkpoint (deep, wet)

    // ---- North approach + goal: the river's source. ----
    // North of the crossing it opens back to grass; a lone far wind marks the
    // top edge. The goal is the spring the whole river flows from -- a strong
    // water emitter at the north end. Reaching it wins.
    addTree({ -6.0f, 24.0f });  addTree({ 8.0f, 28.0f });
    addTree({ -10.0f, 34.0f }); addTree({ 5.0f, 36.0f });
    addEmitter({ 8.0f, 28.0f, 3.0f }, loopRule(bird, 0.7f, 0.06f, true));
    addEmitter({ -20.0f, 42.0f, 12.0f }, loopRule(wind, 0.18f, 0.15f, false));
    addBug({ -2.0f, 30.0f, 1.6f });
    // A short wall guarding the final approach, so the goal isn't a straight run.
    addWall({ -6.0f, 32.0f }, { 10.0f, 32.0f }, 8.0f, 2.0f);
    {
        Emitter e; e.pos = { 0.0f, 40.0f, 0.3f }; e.id = s.takeId();
        SoundRule r = loopRule(water, 1.0f, 0.12f, true);
        r.audibleRadius = 42.0f;   // the source carries -- a beacon at the top
        e.sounds.rule(SoundSlot::Loop) = r;
        e.isGoal = true;
        s.emitters.push_back(std::move(e));
    }

    // ---- The Stalker: owns the river crossing between south and north. ----
    // Fixed home at the crossing so the danger zone is learnable; it patrols the
    // banks and doorways, repelled only where the music reaches.
    {
        Stalker st;
        st.home = { 0.0f, 8.0f, 1.2f };
        st.pos = st.home;
        st.patrolRadius = 12.0f;
        st.id = s.takeId();
        st.sounds.rule(SoundSlot::Loop) = makeStalkerLoopRule(lib);
        s.stalkers.push_back(st);
    }

    return s;
}
