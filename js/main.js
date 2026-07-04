// Boot + glue for the audio_game web build.
//
// Responsibilities (kept deliberately thin -- all game logic is in WASM):
//   1. Load the Emscripten module (audiogame.js, MODULARIZE, EXPORT_NAME).
//   2. On a user gesture (required by browser autoplay policy) create the
//      AudioContext, register its handle with WASM, and start the audio worklet.
//   3. Size the canvas + WebGL drawing buffer; keep it in sync on resize.
//   4. Forward DOM keyboard/mouse/pointer-lock events to the WASM input_* API,
//      mapping DOM KeyboardEvent.code -> SDL3 SDLK_* values.
//   5. Run the frame loop: requestAnimationFrame -> wasm_tick(dt).

//////////////////// DOM code -> SDL3 SDLK_* keycode map ////////////////////
// Must match webgame/src/sdl_keys_compat.h. Printable keys == ASCII (lowercase
// letters); named keys == (scancode | SDLK_SCANCODE_MASK). Only the keys the
// game/DevMode actually read are mapped; anything else is ignored.
const SDLK_SCANCODE_MASK = 1 << 30;
const SC = (n) => (n | SDLK_SCANCODE_MASK);
const KEYMAP = {
  // movement (latched in WASM)
  KeyW: 0x77, KeyA: 0x61, KeyS: 0x73, KeyD: 0x64,
  Space: 0x20,
  ShiftLeft: SC(225), ShiftRight: SC(229), ControlLeft: SC(224),
  // dev-mode letters
  KeyB: 0x62, KeyC: 0x63, KeyE: 0x65, KeyF: 0x66, KeyG: 0x67,
  KeyK: 0x6b, KeyL: 0x6c, KeyN: 0x6e, KeyO: 0x6f, KeyP: 0x70,
  KeyQ: 0x71, KeyR: 0x72, KeyT: 0x74, KeyU: 0x75, KeyV: 0x76, KeyX: 0x78,
  Digit1: 0x31, Digit2: 0x32, Digit3: 0x33, Digit4: 0x34,
  BracketLeft: 0x5b, BracketRight: 0x5d,
  // named keys
  Enter: SC(40), Escape: SC(41),
  ArrowRight: SC(79), ArrowLeft: SC(80), ArrowDown: SC(81), ArrowUp: SC(82),
};

// Top-level game keys the native main.cpp handled inline (not via DevMode).
// Delivered through dedicated toggle exports so WASM dispatch order matches.
const TOGGLE = {
  F1: 'toggle_stats',
  F2: 'toggle_dev',
  F3: 'toggle_visual',
  Tab: 'toggle_fullbright',
};

async function boot() {
  const overlay = document.getElementById('gate');
  const status = document.getElementById('status');
  const canvas = document.getElementById('canvas');

  status.textContent = 'loading module...';

  // MODULARIZE factory. Point Emscripten at our canvas for its GL calls.
  // locateFile: the .js lives in dist/ but the page is at the site root, so
  // Emscripten's default (resolve audiogame.data/.wasm next to the PAGE) 404s.
  // Rewrite those two package names to the dist/ folder. Worker/worklet script
  // URLs are already absolute and must NOT be rewritten.
  const Module = await createAudioGame({
    canvas,
    locateFile: (path, prefix) => {
      if (path === 'audiogame.wasm' || path === 'audiogame.data')
        return 'dist/' + path;
      return prefix + path;
    },
    print: (t) => console.log(t),
    printErr: (t) => console.warn(t),
  });

  // Bind the exported functions once.
  const init        = Module.cwrap('wasm_init', null, ['number']);
  const tick        = Module.cwrap('wasm_tick', null, ['number']);
  const resize      = Module.cwrap('wasm_resize', null, ['number', 'number']);
  const createAudio = Module.cwrap('wasm_create_audio', 'number', []);
  const startAudio  = Module.cwrap('wasm_start_audio', null, []);
  const resumeAudio = Module.cwrap('wasm_resume_audio', null, []);
  const cpuLoad     = Module.cwrap('wasm_cpu_load', 'number', []);
  const inKey       = Module.cwrap('input_key', null, ['number', 'number', 'number']);
  const inMouse     = Module.cwrap('input_mouse', null, ['number', 'number']);
  const inWheel     = Module.cwrap('input_wheel', null, ['number']);
  const inMouseBtn  = Module.cwrap('input_mouse_button', null, ['number', 'number']);
  const inPtrLock   = Module.cwrap('input_pointerlock', null, ['number']);
  const inPing      = Module.cwrap('input_ping', null, []);
  const toggles = {
    toggle_stats:      Module.cwrap('input_toggle_stats', null, []),
    toggle_dev:        Module.cwrap('input_toggle_dev', null, []),
    toggle_visual:     Module.cwrap('input_toggle_visual', null, []),
    toggle_fullbright: Module.cwrap('input_toggle_fullbright', null, []),
  };

  //////////////////// canvas sizing ////////////////////
  // Size the WebGL drawing buffer to the canvas's laid-out size * DPR. We read
  // the element's real box from getBoundingClientRect (falling back to the
  // viewport) rather than clientWidth, which can read 0/stale before the first
  // layout pass -- the cause of the "rendered into the top-left, blank on the
  // right until you resize" bug on initial load.
  function fit() {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const rect = canvas.getBoundingClientRect();
    const cssW = rect.width || window.innerWidth;
    const cssH = rect.height || window.innerHeight;
    const w = Math.max(1, Math.floor(cssW * dpr));
    const h = Math.max(1, Math.floor(cssH * dpr));
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    resize(canvas.width, canvas.height);
  }
  window.addEventListener('resize', fit);
  // Layout can settle a frame or two after the module loads / the gate hides;
  // a ResizeObserver re-fits whenever the canvas box actually changes, so we
  // don't depend on a single well-timed fit() call.
  if (typeof ResizeObserver !== 'undefined') {
    new ResizeObserver(fit).observe(canvas);
  }

  //////////////////// input wiring ////////////////////
  window.addEventListener('keydown', (e) => {
    if (e.code in TOGGLE) { e.preventDefault(); toggles[TOGGLE[e.code]](); return; }
    const kc = KEYMAP[e.code];
    if (kc === undefined) return;
    e.preventDefault();
    inKey(kc, 1, e.repeat ? 1 : 0);
    // E is the echo probe in play (WASM ignores it while dev mode owns the key).
    // Still forwarded via inKey above so DevMode keeps its E binding.
    if (e.code === 'KeyE' && !e.repeat) inPing();
  });
  window.addEventListener('keyup', (e) => {
    const kc = KEYMAP[e.code];
    if (kc === undefined) return;
    inKey(kc, 0, 0);
  });

  // Pointer lock for mouse-look (native used relative mouse mode).
  canvas.addEventListener('click', () => {
    if (document.pointerLockElement !== canvas) canvas.requestPointerLock();
  });
  document.addEventListener('pointerlockchange', () => {
    inPtrLock(document.pointerLockElement === canvas ? 1 : 0);
  });
  window.addEventListener('mousemove', (e) => {
    if (document.pointerLockElement === canvas)
      inMouse(e.movementX, e.movementY);
  });
  // Track held button state (down/up) so DevMode drag-paint works; the down
  // edge also drives the discrete click dev action (inside input_mouse_button).
  canvas.addEventListener('mousedown', (e) => {
    if (document.pointerLockElement === canvas) inMouseBtn(e.button, 1);
  });
  window.addEventListener('mouseup', (e) => {
    inMouseBtn(e.button, 0);
  });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  canvas.addEventListener('wheel', (e) => {
    if (document.pointerLockElement === canvas) { e.preventDefault(); inWheel(-Math.sign(e.deltaY)); }
  }, { passive: false });

  //////////////////// gesture -> audio + start ////////////////////
  async function start() {
    overlay.removeEventListener('click', start);
    overlay.style.display = 'none';
    status.textContent = 'starting...';

    // The AudioContext is created inside WASM (Emscripten-recommended path):
    // wasm_create_audio() makes it and returns its real sample rate, which the
    // engine is then built at. Creating it here, still inside the click's
    // handler, satisfies the browser autoplay policy.
    const rate = createAudio();
    if (!rate) { status.textContent = 'audio init failed'; return; }

    fit();
    init(rate);          // build the world at the context's rate; creates the
                         // GL context, so the canvas buffer must be sized first
    fit();               // re-fit after context creation, in case init reset it
    startAudio();        // boot worklet thread + node (async)
    resumeAudio();       // resume the context so sound flows

    // The audio CPU meter is a no-op on web (the worklet thread can't use
    // steady_clock; consumeCpuLoad() always returns 0 -- see AudioWorld::render).
    // So we don't display it; clear the boot status once the loop is running.
    if (status) status.textContent = '';

    // Frame loop.
    let prev = performance.now();
    let firstFrame = true;
    function frame(now) {
      const dt = (now - prev) / 1000;
      prev = now;
      if (firstFrame) { fit(); firstFrame = false; }  // final size guard
      tick(dt);
      requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
  }

  overlay.addEventListener('click', start);
  status.textContent = 'ready — click to enter';
}

boot().catch((err) => {
  console.error(err);
  const status = document.getElementById('status');
  if (status) status.textContent = 'failed to load: ' + err;
});
