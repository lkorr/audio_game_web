// Minimal pthread stubs for the WASM_WORKERS build.
//
// -sAUDIO_WORKLET requires -sWASM_WORKERS, whose prebuilt libc++ variant
// (libc++-ww) still references pthread_mutex_* / pthread_cond_* from its
// std::call_once implementation (pulled in transitively -- not by our code).
// WASM_WORKERS is a lightweight threading model WITHOUT a pthread runtime, so
// those symbols are undefined at link.
//
// We satisfy them with no-op stubs. This is safe for THIS program: the only
// call_once-driven paths (libc++ one-time init) execute during module startup
// on the main thread, in a single-threaded context, before the audio worklet
// thread exists. No real mutual exclusion is required there. The audio thread
// communicates with the main thread solely through AudioWorld's own lock-free
// ParamBuffer, which uses C++ atomics, not these pthread primitives.
//
// If future code introduces genuine cross-thread std::mutex contention, replace
// these with real implementations (or switch the build to full -pthread).

int pthread_mutex_lock(void* m)   { (void)m; return 0; }
int pthread_mutex_unlock(void* m) { (void)m; return 0; }
int pthread_mutex_trylock(void* m){ (void)m; return 0; }
int pthread_cond_wait(void* c, void* m)      { (void)c; (void)m; return 0; }
int pthread_cond_broadcast(void* c)          { (void)c; return 0; }
int pthread_cond_signal(void* c)             { (void)c; return 0; }
