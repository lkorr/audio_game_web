#pragma once
// Lock-free single-writer / single-reader handoff of EngineParams from the
// game thread to the audio thread (FIRST_STEP_PLAN §3): double buffer plus an
// atomic index flip. The writer fills the inactive slot then publishes it; the
// reader copies the active slot once at block start. The engine's internal
// smoothers absorb the 60 Hz -> block-rate quantization.
//
// Known (accepted) limitation of the plain double buffer: if the writer flips
// twice while the reader is mid-copy the reader can observe a torn snapshot.
// At 60 Hz writes vs ~microsecond copies this is vanishingly rare, and the
// consequence is one block of slightly inconsistent positions, absorbed by the
// smoothers.

#include "xyzpan/Types.h"
#include <atomic>

class ParamBuffer {
public:
    // Game thread only.
    void write(const xyzpan::EngineParams& p) {
        const int next = 1 - active_.load(std::memory_order_acquire);
        slots_[next] = p;
        active_.store(next, std::memory_order_release);
    }

    // Audio thread only. Copies the latest complete snapshot.
    void read(xyzpan::EngineParams& out) const {
        out = slots_[active_.load(std::memory_order_acquire)];
    }

private:
    xyzpan::EngineParams slots_[2];
    std::atomic<int> active_{0};
};
