#include "FramePacer.h"

#include <algorithm>

namespace eacp::Threads
{
void FramePacer::setMaxFps(int fpsToUse)
{
    fps = std::max(0, fpsToUse);
    minInterval = fps > 0 ? 1.0 / fps : 0.0;
}

int FramePacer::maxFps() const
{
    return fps;
}

void FramePacer::reset()
{
    started = false;
}

bool FramePacer::advance(double seconds)
{
    const auto interval = minInterval.load();

    if (interval <= 0.0)
    {
        started = false;
        return true;
    }

    if (!started)
    {
        started = true;
        accumulated = 0.0;
        return true;
    }

    accumulated += seconds;

    if (accumulated + seconds * 0.5 < interval)
        return false;

    accumulated = std::min(accumulated - interval, interval * 0.5);
    return true;
}
} // namespace eacp::Threads
