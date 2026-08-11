#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>
#include <string>

namespace eacp::GPU
{
// One labelled pass, from the start of its first stage to the end of its last.
struct PassTiming
{
    std::string label;
    double milliseconds = 0.0;
};

// One frame's GPU cost: every labelled pass in encode order, and the command
// buffer end to end. Always describes a frame already displayed, timestamps
// only existing once the GPU has run the work.
struct FrameTimings
{
    Vector<PassTiming> passes;

    double milliseconds = 0.0;

    // Device::frameIndex() when this frame was encoded; zero until the first
    // frame's numbers come back.
    std::uint64_t frameIndex = 0;
};
} // namespace eacp::GPU
