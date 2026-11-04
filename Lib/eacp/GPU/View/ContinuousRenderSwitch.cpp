#include "ContinuousRenderSwitch.h"

#include <utility>

namespace eacp::GPU
{
ContinuousRenderSwitch::ContinuousRenderSwitch(Callback startLinkToUse,
                                               Callback stopLinkToUse)
    : startLink(std::move(startLinkToUse))
    , stopLink(std::move(stopLinkToUse))
{
}

void ContinuousRenderSwitch::setRequested(bool requestedToUse)
{
    requested = requestedToUse;
    applyChange();
}

void ContinuousRenderSwitch::setWindowVisible(bool visible)
{
    windowVisible = visible;
    applyChange();
}

bool ContinuousRenderSwitch::isRunning() const
{
    return running;
}

void ContinuousRenderSwitch::applyChange()
{
    const auto shouldRun = requested && windowVisible;

    if (shouldRun == running)
        return;

    running = shouldRun;

    if (running)
        startLink();
    else
        stopLink();
}
} // namespace eacp::GPU
