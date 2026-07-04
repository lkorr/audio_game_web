#!/usr/bin/env bash
# Build the web version. Requires the Emscripten SDK on PATH (emcmake/emmake).
#
#   source /path/to/emsdk/emsdk_env.sh   # once per shell
#   ./build.sh
#
# Output lands in webgame/dist/ (audiogame.js, .wasm, .data). Serve the webgame
# folder over HTTP (see README) -- opening index.html via file:// will NOT work.
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v emcmake >/dev/null 2>&1; then
  echo "error: emcmake not found. Install the Emscripten SDK and run:" >&2
  echo "       source /path/to/emsdk/emsdk_env.sh" >&2
  exit 1
fi

BUILD_DIR="build-em"
emcmake cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
emmake cmake --build "$BUILD_DIR" -j

echo
echo "Build complete. Artifacts in dist/:"
ls -la dist/ 2>/dev/null || true
echo
echo "Serve locally with:  python -m http.server 8000   (then open http://localhost:8000/)"
