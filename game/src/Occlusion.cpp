#include "Occlusion.h"
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr float kSpeedOfSound = 343.0f;   // m/s
constexpr float kMaxCutoffHz = 20000.0f;
constexpr float kEps = 1e-4f;
constexpr float kInf = std::numeric_limits<float>::infinity();

// ---- Segment-vs-volume clipping. Segment p(t) = a + t*(b-a), t in [0,1]. ----

bool segVsBox(const glm::vec3& a, const glm::vec3& b,
              const glm::vec3& c, const glm::vec3& h,
              float& tEnter, float& tExit) {
    float t0 = 0.0f, t1 = 1.0f;
    const glm::vec3 d = b - a;
    for (int i = 0; i < 3; ++i) {
        const float lo = c[i] - h[i], hi = c[i] + h[i];
        if (std::abs(d[i]) < 1e-9f) {
            if (a[i] < lo || a[i] > hi) return false;
        } else {
            float u = (lo - a[i]) / d[i];
            float v = (hi - a[i]) / d[i];
            if (u > v) std::swap(u, v);
            t0 = std::max(t0, u);
            t1 = std::min(t1, v);
        }
        if (t0 >= t1) return false;
    }
    tEnter = t0;
    tExit = t1;
    return t1 - t0 > 1e-6f;
}

bool segVsCylinder(const glm::vec3& a, const glm::vec3& b,
                   const glm::vec2& c, float r, float zMin, float zMax,
                   float& tEnter, float& tExit) {
    // Infinite vertical cylinder (2D circle), then clip by the z slab.
    const glm::vec2 p(a.x, a.y);
    const glm::vec2 d(b.x - a.x, b.y - a.y);
    const glm::vec2 m = p - c;
    const float A = glm::dot(d, d);
    const float B = 2.0f * glm::dot(m, d);
    const float C = glm::dot(m, m) - r * r;
    float t0, t1;
    if (A < 1e-12f) {
        if (C > 0.0f) return false;   // vertical segment outside the circle
        t0 = 0.0f;
        t1 = 1.0f;
    } else {
        const float disc = B * B - 4.0f * A * C;
        if (disc <= 0.0f) return false;
        const float s = std::sqrt(disc);
        t0 = (-B - s) / (2.0f * A);
        t1 = (-B + s) / (2.0f * A);
    }
    const float dz = b.z - a.z;
    float u0 = 0.0f, u1 = 1.0f;
    if (std::abs(dz) < 1e-9f) {
        if (a.z < zMin || a.z > zMax) return false;
    } else {
        u0 = (zMin - a.z) / dz;
        u1 = (zMax - a.z) / dz;
        if (u0 > u1) std::swap(u0, u1);
    }
    tEnter = std::max({t0, u0, 0.0f});
    tExit = std::min({t1, u1, 1.0f});
    return tExit - tEnter > 1e-6f;
}

bool occluderContains(const Occluder& o, const glm::vec3& p) {
    constexpr float kMargin = 0.05f;   // emitters sit at/near object centers
    if (o.shape == Occluder::Shape::Box) {
        const glm::vec3 d = glm::abs(p - o.center);
        return d.x <= o.half.x + kMargin && d.y <= o.half.y + kMargin
            && d.z <= o.half.z + kMargin;
    }
    const glm::vec2 d(p.x - o.center.x, p.y - o.center.y);
    return glm::dot(d, d) <= (o.radius + kMargin) * (o.radius + kMargin)
        && p.z >= o.zMin - kMargin && p.z <= o.zMax + kMargin;
}

// ---- 2D lateral wraps: shortest path from l to s around the footprint. ----

// Circle: tangent + arc + tangent, for BOTH ways around (out[0] near side,
// out[1] far side) -- sound recombines from both, which is what makes a thin
// trunk transparent at range. kInf if an endpoint is inside the trunk column
// at any height (no lateral detour exists; the top path covers it).
void circleWrapLens(const glm::vec2& l, const glm::vec2& s,
                    const glm::vec2& c, float r, float out[2]) {
    out[0] = out[1] = kInf;
    const float dL = glm::length(l - c), dS = glm::length(s - c);
    if (dL <= r + kEps || dS <= r + kEps) return;
    const float tans = std::sqrt(std::max(0.0f, dL * dL - r * r))
                     + std::sqrt(std::max(0.0f, dS * dS - r * r));
    const float cosT = std::clamp(glm::dot(l - c, s - c) / (dL * dS), -1.0f, 1.0f);
    const float tangentAng = std::acos(std::clamp(r / dL, 0.0f, 1.0f))
                           + std::acos(std::clamp(r / dS, 0.0f, 1.0f));
    const float theta = std::acos(cosT);
    out[0] = tans + r * std::max(0.0f, theta - tangentAng);
    out[1] = tans + r * std::max(0.0f, 2.0f * 3.14159265f - theta - tangentAng);
}

// Strict-interior 2D segment-vs-rect test (rect shrunk slightly so paths may
// graze edges/corners).
bool segHitsRect2D(const glm::vec2& a, const glm::vec2& b,
                   const glm::vec2& c, const glm::vec2& h) {
    float t0 = 0.0f, t1 = 1.0f;
    const glm::vec2 d = b - a;
    for (int i = 0; i < 2; ++i) {
        const float hs = std::max(0.0f, h[i] - 1e-3f);
        const float lo = c[i] - hs, hi = c[i] + hs;
        if (std::abs(d[i]) < 1e-9f) {
            if (a[i] < lo || a[i] > hi) return false;
        } else {
            float u = (lo - a[i]) / d[i];
            float v = (hi - a[i]) / d[i];
            if (u > v) std::swap(u, v);
            t0 = std::max(t0, u);
            t1 = std::min(t1, v);
        }
        if (t0 >= t1) return false;
    }
    return t1 - t0 > 1e-6f;
}

// Rectangle: per side of the line l->s, enumerate ordered corner
// subsequences (at most 3 corners per side, 7 subsets) and keep the shortest
// chain whose legs don't cut back through the rect. Both sides reported
// separately (out[0]/out[1]); kInf where no chain exists.
void boxWrapLens2D(const glm::vec2& l, const glm::vec2& s,
                   const glm::vec2& c, const glm::vec2& h, float out[2]) {
    out[0] = out[1] = kInf;
    // Endpoint inside the footprint: no lateral detour (top path covers it).
    if ((std::abs(l.x - c.x) <= h.x && std::abs(l.y - c.y) <= h.y) ||
        (std::abs(s.x - c.x) <= h.x && std::abs(s.y - c.y) <= h.y))
        return;

    const glm::vec2 corners[4] = {
        { c.x - h.x, c.y - h.y }, { c.x + h.x, c.y - h.y },
        { c.x + h.x, c.y + h.y }, { c.x - h.x, c.y + h.y },
    };
    const glm::vec2 dir = s - l;
    for (int side = 0; side < 2; ++side) {
        glm::vec2 pts[4];
        int n = 0;
        for (const glm::vec2& p : corners) {
            const float cross = dir.x * (p.y - l.y) - dir.y * (p.x - l.x);
            if ((side == 0) ? (cross > 0.0f) : (cross < 0.0f)) pts[n++] = p;
        }
        if (n == 0) continue;
        // Order along the travel direction.
        std::sort(pts, pts + n, [&](const glm::vec2& a, const glm::vec2& b) {
            return glm::dot(a - l, dir) < glm::dot(b - l, dir);
        });
        for (int mask = 1; mask < (1 << n); ++mask) {
            glm::vec2 prev = l;
            float len = 0.0f;
            bool valid = true;
            for (int i = 0; i < n && valid; ++i) {
                if (!(mask & (1 << i))) continue;
                valid = !segHitsRect2D(prev, pts[i], c, h);
                len += glm::length(pts[i] - prev);
                prev = pts[i];
            }
            if (valid) valid = !segHitsRect2D(prev, s, c, h);
            len += glm::length(s - prev);
            if (valid) out[side] = std::min(out[side], len);
        }
    }
}

} // namespace

std::vector<Occluder> buildOccluders(const Scene& scene) {
    std::vector<Occluder> out;
    out.reserve(scene.trees.size() + scene.boulders.size());
    for (const Tree& t : scene.trees) {
        Occluder o;
        o.shape = Occluder::Shape::Cylinder;
        o.center = { t.pos.x, t.pos.y, 0.0f };
        o.radius = t.trunkRadius;
        o.zMin = 0.0f;
        o.zMax = t.trunkHeight;
        out.push_back(o);
    }
    for (const Boulder& b : scene.boulders) {
        Occluder o;
        o.shape = Occluder::Shape::Box;
        o.center = b.center;
        o.half = b.half;
        out.push_back(o);
    }
    return out;
}

OcclusionResult computeOcclusion(const glm::vec3& listener, const glm::vec3& source,
                                 const std::vector<Occluder>& occluders,
                                 const OcclusionTuning& tuning) {
    OcclusionResult res;
    const float dist = glm::length(source - listener);
    if (dist < kEps) return res;

    const float lambdaRef = kSpeedOfSound / tuning.refFreqHz;
    const float lambdaFres = kSpeedOfSound / tuning.fresnelHz;

    // Per obstacle: detour-based Maekawa strength, scaled by how solidly the
    // obstacle plugs the Fresnel zone (sequential barriers' dB ducks add; the
    // cutoff is the darkest single obstacle).
    float attenDbSum = 0.0f;
    float cutoffMin = kMaxCutoffHz;
    for (const Occluder& o : occluders) {
        // An object never occludes its own emitter (boulder impact sounds
        // emit from the AABB center, tree sounds from inside the trunk).
        if (occluderContains(o, source) || occluderContains(o, listener)) continue;

        float t0, t1;
        const bool hit = (o.shape == Occluder::Shape::Box)
            ? segVsBox(listener, source, o.center, o.half, t0, t1)
            : segVsCylinder(listener, source, glm::vec2(o.center.x, o.center.y),
                            o.radius, o.zMin, o.zMax, t0, t1);
        if (!hit) continue;

        const glm::vec3 p0 = listener + (source - listener) * t0;
        const glm::vec3 p1 = listener + (source - listener) * t1;
        const float across = glm::length(p1 - p0);   // e: width along the path

        // Escape paths: around the footprint both ways (2D wrap plus the
        // endpoints' height difference -- the wrap happens in the horizontal
        // plane; the climb is shared with the direct path), and over the top
        // (up to the entry-side top edge, across, down).
        const glm::vec2 l2(listener.x, listener.y), s2(source.x, source.y);
        float wraps[2];
        if (o.shape == Occluder::Shape::Box)
            boxWrapLens2D(l2, s2, glm::vec2(o.center.x, o.center.y),
                          glm::vec2(o.half.x, o.half.y), wraps);
        else
            circleWrapLens(l2, s2, glm::vec2(o.center.x, o.center.y), o.radius, wraps);

        const float zTop = (o.shape == Occluder::Shape::Box)
            ? o.center.z + o.half.z : o.zMax;
        const glm::vec3 e0(p0.x, p0.y, zTop), e1(p1.x, p1.y, zTop);
        const float deltaTop = glm::length(e0 - listener) + glm::length(e1 - e0)
                             + glm::length(source - e1) - dist;

        float deltas[3];
        int nPaths = 0;
        const float dz = source.z - listener.z;
        for (int k = 0; k < 2; ++k)
            if (wraps[k] < kInf)
                deltas[nPaths++] = std::max(0.0f,
                    std::sqrt(wraps[k] * wraps[k] + dz * dz) - dist);
        deltas[nPaths++] = std::max(0.0f, deltaTop);

        float deltaMin = deltas[0];
        for (int k = 1; k < nPaths; ++k) deltaMin = std::min(deltaMin, deltas[k]);
        if (deltaMin < 1e-5f) continue;   // grazing the silhouette

        // Solidity: how completely the obstacle plugs the Fresnel zone. Each
        // escape path is open in proportion to 1 - N at the Fresnel reference
        // frequency (N = 2*delta/lambda; the path-detour Fresnel number IS
        // the squared obstacle-size / zone-radius ratio). Openness from all
        // paths adds -- a thin trunk far from both endpoints lets the wave
        // recombine around both sides and overhead, and the effect fades out;
        // a wall keeps every path pinched and stays fully solid.
        float open = 0.0f;
        for (int k = 0; k < nPaths; ++k)
            open += std::max(0.0f, 1.0f - 2.0f * deltas[k] / lambdaFres);
        float solidity = 1.0f - std::min(1.0f, open);

        // Shadow healing: the shadow cast past an obstacle of effective width
        // w refills by diffraction within ~w^2/lambda meters. Once the far
        // endpoint is beyond that, the wave has recombined -- a trunk's
        // shadow heals within meters, a wall's persists for hundreds. The
        // effective width is the narrower of the lateral silhouette and the
        // headroom over the top (sound heals over a low boulder first).
        const float d1 = t0 * dist, d2 = (1.0f - t1) * dist;
        const glm::vec2 dir2(source.x - listener.x, source.y - listener.y);
        const float len2 = glm::length(dir2);
        float wLat = 2.0f * o.radius;
        if (o.shape == Occluder::Shape::Box && len2 > kEps) {
            const glm::vec2 perp(-dir2.y / len2, dir2.x / len2);
            wLat = 2.0f * (std::abs(perp.x) * o.half.x + std::abs(perp.y) * o.half.y);
        }
        const float zMid = listener.z + (source.z - listener.z) * 0.5f * (t0 + t1);
        const float wVert = 2.0f * std::max(kEps, zTop - zMid);
        const float wEff = std::max(kEps, std::min(wLat, wVert));
        const float heal = std::min(1.0f,
            lambdaFres * std::max(d1, d2) / (wEff * wEff));
        solidity *= 1.0f - heal;
        if (solidity <= 0.0f) continue;

        // ISO 9613-2 thick-barrier factor: 1 for thin edges, ->3 when the
        // path across the obstacle is long compared to the wavelength.
        float c3 = 1.0f;
        if (tuning.thickness && across > 1e-3f) {
            const float q = 5.0f * lambdaRef / across;
            c3 = (1.0f + q * q) / (1.0f / 3.0f + q * q);
        }

        // Maekawa at the reference frequency -> dB duck, scaled by solidity.
        // The centimeter ramp masks Maekawa's ~4.8 dB step the instant line
        // of sight breaks (the audio thread also smooths in time).
        const float fresnelN = 2.0f * deltaMin / lambdaRef;
        attenDbSum += 10.0f * std::log10(3.0f + 20.0f * fresnelN * c3)
                    * tuning.attenScale * solidity
                    * std::min(1.0f, deltaMin / 0.01f);

        // Cutoff: where attenuation exceeds the low band by ~6 dB, i.e.
        // N(f)*C3 = 0.45 => fc = 0.45*c / (2*delta*C3); then partial solidity
        // releases it toward open in log-frequency.
        float fc = std::clamp(
            tuning.cutoffScale * 0.45f * kSpeedOfSound / (2.0f * deltaMin * c3),
            std::min(tuning.minCutoffHz, kMaxCutoffHz), kMaxCutoffHz);
        fc = kMaxCutoffHz * std::pow(fc / kMaxCutoffHz, solidity);
        cutoffMin = std::min(cutoffMin, fc);
    }
    if (attenDbSum <= 0.0f && cutoffMin >= kMaxCutoffHz) return res;

    res.gain = std::pow(10.0f, -std::min(attenDbSum, tuning.maxAttenDb) / 20.0f);
    res.cutoffHz = cutoffMin;
    return res;
}
