#pragma once
// Geometric sound occlusion (game thread). When scene geometry blocks the
// straight emitter->listener path, sound still arrives by diffracting around
// the obstacle's edges; the detour costs level, and costs more at high
// frequencies, which is why occluded sounds get muffled.
//
// Model: Maekawa's knife-edge approximation with the ISO 9613-2 thick-barrier
// (double edge) correction.
//   Fresnel number  N = 2*delta / lambda        delta = detour path difference
//   Attenuation     A = 10*log10(3 + 20*N*C3)   dB, capped at 25 dB
//   C3 = (1 + (5*lambda/e)^2) / (1/3 + (5*lambda/e)^2)   e = width across the
//        obstacle along the path; 1 for thin edges, ->3 (~+5 dB) when e >> lambda
// Since N grows with frequency, A is a frequency tilt; we collapse it to the
// game-standard pair (broadband gain, low-pass cutoff):
//   gain   from A evaluated at 250 Hz
//   cutoff where A exceeds the low band by ~6 dB: N*C3 = 0.45
//          => fc ~= 0.45*c / (2*delta*C3), clamped to [300 Hz, 20 kHz]
// The detour delta per obstacle is the cheapest of: around the left edge,
// around the right edge (2D wrap), or over the top -- so a wide low boulder
// muffles less than a narrow tall one at the same depth, and obstacle width
// lengthens the detour exactly as it should.
//
// Fresnel-zone solidity: the wavefront occupies an ellipsoid of radius
// F1 = sqrt(lambda*d1*d2/(d1+d2)) around the sightline, growing with distance
// to BOTH endpoints. An obstacle small next to that zone barely shadows --
// the wave recombines around both sides and overhead. Each escape path is
// open in proportion to 1 - N(path) at a reference frequency (its detour
// Fresnel number is exactly the squared obstacle/zone size ratio), the
// opennesses add, and 1 - total scales the whole effect. Step far enough
// from a tree (or move the source away from it) and its muffling fades to
// nothing; a wall keeps every path pinched and stays fully solid.
//
// computeOcclusion() is pure geometry + math, called per active voice per
// frame; Source carries the result to the audio thread and smooths it there.

#include "Scene.h"
#include <glm/vec3.hpp>
#include <vector>

struct Occluder {
    enum class Shape : int { Cylinder, Box };
    Shape shape = Shape::Box;
    glm::vec3 center{0.0f};   // Box: AABB center. Cylinder: axis x/y (z unused)
    glm::vec3 half{0.5f};     // Box: AABB half extents
    float radius = 0.0f;      // Cylinder only
    float zMin = 0.0f;        // Cylinder vertical extent
    float zMax = 0.0f;
};

struct OcclusionResult {
    float cutoffHz = 20000.0f;  // low-pass target; 20 kHz = open
    float gain = 1.0f;          // broadband attenuation, linear
};

// Live-tunable knobs (dev panel OCCLUSION category; AudioWorld fills this
// from EngineTuning every frame). Defaults match the physics derivation.
struct OcclusionTuning {
    float attenScale = 1.0f;    // multiplies the Maekawa dB duck
    float maxAttenDb = 25.0f;   // cap (ISO 9613-2 double-diffraction limit)
    float cutoffScale = 1.0f;   // multiplies the low-pass cutoff (<1 = darker)
    float minCutoffHz = 300.0f; // fully-occluded cutoff floor
    float refFreqHz = 250.0f;   // frequency the broadband gain is evaluated at
    // Frequency the Fresnel-zone solidity and shadow-healing tests run at.
    // Higher = shorter wavelength = obstacles count as solid from further
    // away and shadows heal more slowly (more occlusion overall); lower =
    // everything more transparent. ~6 kHz keeps a trunk strong up close but
    // transparent past ~10 m.
    float fresnelHz = 6000.0f;
    bool thickness = true;      // apply the C3 wide-obstacle factor
};

// Tree trunks (cylinders) and boulders (AABBs). Canopies are foliage --
// acoustically near-transparent -- so they are not occluders.
std::vector<Occluder> buildOccluders(const Scene& scene);

OcclusionResult computeOcclusion(const glm::vec3& listener, const glm::vec3& source,
                                 const std::vector<Occluder>& occluders,
                                 const OcclusionTuning& tuning = {});
