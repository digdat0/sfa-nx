// Not part of the SFA-NX build: only Melee-NX's Aurora patches (aurora-switch-cache-recovery) include
// this file, and build.sh does not apply them to Foxhollow's Aurora. Kept with them for reference.
#pragma once

#include <cstdint>
#include <limits>

namespace melee_nx {

// Protected by Aurora's pipeline mutex. Keep usage metadata even while the
// worker has removed a descriptor from the queue to compile it.
struct PipelineRequestState {
    uint32_t firstFrameUsed = std::numeric_limits<uint32_t>::max();
    bool known = false;
    bool compiling = false;
    // The stored descriptor is already marked as used on this device, so it
    // loads ahead of untouched seed entries.
    bool deviceUsed = false;

    bool remember(uint32_t frame, bool persist) noexcept {
        const bool changed = !known || frame < firstFrameUsed;
        if (changed)
            firstFrameUsed = frame;
        known = true;
        return persist && changed;
    }

    // True once per descriptor, the first time a draw needs it: the row must be
    // rewritten even though its first-use frame did not change.
    bool promote() noexcept {
        const bool first = !deviceUsed;
        deviceUsed = true;
        return first;
    }
};

} // namespace melee_nx
