# Vendored Engine Snapshot

Copied verbatim from `C:\Users\Luke\Desktop\programming\xyzpan\engine\` (include/, src/, CMakeLists.txt).

- Source commit: `e5782fb3f3f17ede70bac43c2d66017373609f70` (xyzpan, 2026-06-10)
- Copied: 2026-06-12
- `cmake/CPM.cmake` also copied from xyzpan.

Do not modify files under `engine/` until the M5 voice refactor (see FIRST_STEP_PLAN.md §5), which is the designated fork point. Until then, upstream fixes re-sync by re-copying — **and must re-apply the local modifications below.**

## Local modifications (post-copy)

1. **Floor bounce factor override** (2026-06-12). The game needs floor bounce
   driven by the source's height above the ground plane, not the
   listener-relative elevation angle — not expressible via params upstream.
   - `include/xyzpan/Types.h`: new `EngineParams::floorBounceFactor` field
     (default `-1` = legacy elevation behavior; `[0,1]` = caller-supplied
     closeness to the floor, 1 = on the floor).
   - `src/Engine.cpp`: both floor `processSample` call sites (mono path and
     `processNodeSample`) use `1 - floorBounceFactor` instead of the
     elevation factor when the override is `>= 0`. Grep `LOCAL MOD`.
