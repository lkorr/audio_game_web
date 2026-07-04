# audio_game — Web build

A self-contained WebAssembly port of the native audio_game. It runs the **same
C++ DSP engine and game logic** as the desktop build; only the platform glue
(window/GL, audio device, input, file I/O) is swapped for browser equivalents.
Drop the built folder onto any static web host and it plays like the exe —
including full dev/build mode.

---

## What's here

| Path | What it is |
|---|---|
| `src/` | Web-only glue: entry point, audio-worklet device, GL loader, input + asset bridges (see per-file headers). |
| `js/main.js` | Boots the WASM module, opens the AudioContext on click, wires input, runs the frame loop. |
| `index.html` | The page served at your route. Canvas + click-to-enter gate. |
| `assets/` | Bundled game data: `scene.txt`, `worlds/`, `audio/*.wav`. Preloaded into the WASM virtual filesystem at build time. |
| `engine/` | The portable C++ DSP engine (`xyzpan`), vendored into this repo. Compiled unchanged. |
| `game/src/` | The portable game logic (scene, sound director, stalker AI, renderer, …), vendored into this repo. |
| `cmake/` | `CPM.cmake` for fetching the header-only deps (glm, miniaudio). |
| `CMakeLists.txt` | Emscripten build. Compiles the local `engine/` + `game/src/*`. |
| `build.sh` / `build.bat` | One-command build. |
| `dist/` | Build output (git-ignored): `audiogame.js`, `audiogame.wasm`, `audiogame.data`. |

This repo is **fully self-contained** — the engine (`engine/`) and portable game
sources (`game/src/*`, minus the native-only `main.cpp`, `AudioDevice.cpp`,
`GLLoader.cpp`) are vendored in, so there is no dependency on any sibling repo.
The web port swaps only the platform glue in `src/`. Two guarded, native-safe
edits live in the shared tree: `engine/CMakeLists.txt` (a WASM SIMD branch) and
`game/src/Renderer.cpp` (a `#ifdef __EMSCRIPTEN__` GLSL-ES shader rewrite).

---

## Prerequisites

The **Emscripten SDK** (`emcc` 3.1.50+ recommended, for the Wasm Audio Worklet
API). Install once:

```bash
git clone https://github.com/emscripten-core/emsdk
cd emsdk && ./emsdk install latest && ./emsdk activate latest
```

Then, in each shell you build from:

```bash
source /path/to/emsdk/emsdk_env.sh      # Windows: emsdk_env.bat
```

CMake 3.25+ and a network connection (first build fetches glm + miniaudio via
CPM, same as the native build).

---

## Build

**Web (WebAssembly):**
```bash
./build.sh            # Windows: build.bat  (wraps emcmake)
```
Artifacts appear in `dist/`. Rebuild any time you change sources or assets.

**Native (desktop -- for live tuning):**
```bash
cmake -B build -G "Visual Studio 17 2022" -A x64      # or your generator
cmake --build build --config Release --target audio_game
./build/game/Release/audio_game --slice
```
The same root `CMakeLists.txt` builds native when configured with a normal
compiler and web when configured via `emcmake` (`build.sh`).

---

## Tuning the game live (`tunables.txt`)

Almost every gameplay/visual knob -- Stalker AI (speeds, thresholds, lunge
range/overshoot, timers, masking, hearing), slice rules (catch/goal radius),
player (speed, radius, eye height, mouse sens), ping loudness, and the
darkness pool / reveal radii -- lives in one file, `tunables.txt`, as
`key = value` lines. The audio-engine params are reachable in the same file via
`engine <key>=<value>`.

Run the **native** build (`audio_game --slice`). On first run it writes a
`tunables.txt` seeded with every key + its default and a comment. Edit it on a
second monitor while the game runs -- it reloads within a frame (no rebuild, no
restart), so you hear/see each change immediately. Delete a line to restore that
knob's built-in default.

**Web:** there is no live filesystem to re-read while playing, so dial values in
on native, then bundle the file for web by copying your tuned `tunables.txt` to
`assets/tunables.txt` and rebuilding -- it is applied once at load.

All defaults live in `game/src/Tunables.{h,cpp}` (the one table `kEntries` is the
source of truth for keys, defaults, and the generated template).

---

## Run locally

Browsers won't load WASM/worklets over `file://` — serve over HTTP. Use the
bundled server, which sets the required headers (see below):

```bash
cd webgame
python serve.py            # http://localhost:8000/  (or: python serve.py 9000)
```

Click to enter (needed to unlock audio and capture the mouse). Put on
headphones — the whole point is binaural.

> **This build requires cross-origin isolation (COOP/COEP headers).** The audio
> runs on a real audio-worklet thread, which Emscripten builds on
> `SharedArrayBuffer`; browsers only expose that when the page is served with:
>
> ```
> Cross-Origin-Opener-Policy: same-origin
> Cross-Origin-Embedder-Policy: require-corp
> ```
>
> `python serve.py` sets both (plain `python -m http.server` does **not** — the
> audio thread silently won't start under it). This is the honest cost of
> real-audio-thread quality; it is not "any static host, zero config." See
> *Serving with the required headers* for deploy targets.

---

## Deploy to your site (`website.com/audiogame`)

Copy these into the folder your server maps to `/audiogame`:

```
audiogame/
  index.html
  js/main.js
  dist/audiogame.js
  dist/audiogame.wasm
  dist/audiogame.data
```

`index.html` uses **relative** paths, so it works at any sub-path. Ensure your
host serves `.wasm` as `application/wasm` (nearly all do).

### Serving with the required headers

The page **must** be served with the two cross-origin-isolation headers (see
*Run locally*). How to set them per host:

- **Netlify / Cloudflare Pages:** the bundled `_headers` file does it — deploy
  it alongside the site.
- **Vercel:** add the headers in `vercel.json` (`headers` array).
- **nginx:** `add_header Cross-Origin-Opener-Policy same-origin; add_header
  Cross-Origin-Embedder-Policy require-corp;` in the `/audiogame` location.
- **Apache:** `Header set` for both in a `.htaccess`.
- **GitHub Pages:** cannot set custom headers — it will **not** work there
  without a proxy/service-worker shim. Use one of the hosts above.

Verify isolation in the browser console: `crossOriginIsolated` should be `true`.
If it's `false`, the headers aren't reaching the page and audio won't start.

---

## Adding your own sound design

The goal is "load it with tons of custom sound files." To add sounds:

1. Drop `.wav`, `.flac`, or `.mp3` files into `webgame/assets/audio/`.
   Naming groups variants: `thud_01.wav` + `thud_02.wav` → one set `thud`
   with two variants (trailing digits are stripped; see `SoundLibrary`).
2. Rebuild (`./build.sh`). Files are decoded in-WASM (miniaudio, decode-only)
   from the preloaded virtual FS — the exact same code path as native.
3. Assign them to objects in dev mode (F2 → edit an object → SOUNDS page), and
   save the world (WORLDS browser, `O`). Saved worlds persist to the virtual
   FS for the session.

**On file count:** loading many files is cheap — they're just input signal fed
through the DSP. What's bounded is how many can play *at once* (voices), not how
many exist. See below.

---

## Scaling up (when you outgrow ~16 simultaneous voices)

Two independent levers, both out of scope for this first build but noted so the
path is clear:

- **Memory / per-voice weight.** Each voice is a full `XYZPanEngine` (~33 MB in
  the current build, dominated by oversized delay lines). The heap is a **fixed
  768 MB** (`INITIAL_MEMORY`, non-growable) — memory growth is intentionally OFF
  because a growable `SharedArrayBuffer` (which `-pthread` forces) produces
  resizable-ArrayBuffer views that WebGL's `bufferData` rejects. So the voice
  count is capped by that fixed heap, not by growth. The engine "M5 fork"
  (right-sizing the 5000 ms / 192 kHz delay lines) drops per-voice cost to
  single-digit MB and is what raises the ceiling; bumping `INITIAL_MEMORY` is
  the crude lever until then (watch mobile limits).
- **CPU / parallelism.** All voices render on one worklet thread. Higher voice
  counts need either the memory fork above (cheaper voices) or multi-threaded
  audio via `SharedArrayBuffer` — which **does** require COOP/COEP headers and
  a host you control. That's the deliberate ceiling of the single-worklet
  design chosen here.

---

## Verifying the port (determinism check)

The native build has a `--render-test`: a scripted 10 s walk rendered offline to
`render_test.wav`, used as a regression baseline. The web build runs the **same
engine and game logic**, so the strongest correctness proof is to reproduce that
render under Node and compare:

1. Build a headless Node target of `runRenderTest()` (the logic lives in the
   native `main.cpp`; a small Node entry that calls the same
   `SoundLibrary`/`Scene`/`AudioWorld`/`SoundDirector` sequence and writes the
   WAV to MEMFS — mirrors `runRenderTest` exactly).
2. Run it with `node`, pull the WAV out of MEMFS, and compare against the
   native `render_test.wav` (byte-equal, or FFT-equal within float tolerance).

Equal output ⇒ the DSP pipeline survived the port intact. In-browser, sanity
check by ear: audio pans as you turn, footsteps fire on movement, a bug source
Dopplers past, and the F2 dev overlay / floor painting / world save-load work.

---

## Troubleshooting

- **Silent, but the page loads:** the AudioContext only starts on the click
  gate. If still silent, check the console for `[web-audio] worklet connected`.
  Some browsers need the tab focused.
- **`emcmake not found`:** you didn't `source emsdk_env.sh` in this shell.
- **Black screen:** needs a WebGL2-capable browser (any current Chrome/Firefox/
  Safari). The console prints `GL_VERSION` on boot.
- **404 on `.data`:** you didn't copy `dist/audiogame.data` alongside the `.js`
  and `.wasm`. All three are required.
