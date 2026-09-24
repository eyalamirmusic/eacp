#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace eacp::GPU
{
// How long one labelled pass took on the GPU: the start of its first stage to
// the end of its last.
struct PassTiming
{
    std::string label;
    double milliseconds = 0.0;

    // How many regions were timed under this label: 1 for a pass, the number
    // of dispatches in a total from FrameTimings::totalsByLabel().
    int count = 1;
};

// What one frame cost the GPU - every pass that was given a label, in the order
// they were encoded, and the command buffer end to end.
//
// These describe the recent past, and have to. A timestamp is written by the
// GPU as it runs the work, so the numbers cannot exist until the frame has
// finished, which is a few frames after it was encoded - that being the point
// of having frames in flight at all. `frameIndex` says which frame this
// actually is, and against Device::frameIndex() says how far behind.
//
// A profiler reading these is therefore looking at a frame the user has already
// seen. That is fine for what timings are for: finding which pass costs what,
// over a load that is not changing every frame.
struct FrameTimings
{
    Vector<PassTiming> passes;

    double milliseconds = 0.0;

    // Device::frameIndex() as it was when this frame was encoded. Zero until
    // the first frame's numbers have come back.
    std::uint64_t frameIndex = 0;

    // The passes summed by label, largest first: what a pass timed with
    // TimingScope::EachDispatch reads as a per-kernel profile.
    Vector<PassTiming> totalsByLabel() const
    {
        auto totals = Vector<PassTiming> {};

        for (const auto& pass: passes)
        {
            auto found = std::find_if(totals.begin(),
                                      totals.end(),
                                      [&](const PassTiming& total)
                                      { return total.label == pass.label; });

            if (found == totals.end())
            {
                totals.add(pass);
                continue;
            }

            found->milliseconds += pass.milliseconds;
            found->count += pass.count;
        }

        std::sort(totals.begin(),
                  totals.end(),
                  [](const PassTiming& a, const PassTiming& b)
                  { return a.milliseconds > b.milliseconds; });

        return totals;
    }
};
} // namespace eacp::GPU
