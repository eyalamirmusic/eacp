#pragma once

/** @file
    Starts and stops a GPUView's display link as continuous mode and window
    visibility change.
*/

#include <eacp/Core/Utils/Common.h>

namespace eacp::GPU
{
// Runs a view's display link only while continuous rendering is asked for and
// the view's window can be seen. A window that is minimised, ordered out,
// fully covered or on a sleeping display is never scheduled by the
// compositor, so a frame drawn for it would only wait on the GPU queue.
class ContinuousRenderSwitch
{
public:
    ContinuousRenderSwitch(Callback startLinkToUse, Callback stopLinkToUse);

    void setRequested(bool requested);
    void setWindowVisible(bool visible);

    bool isRunning() const;

private:
    void applyChange();

    Callback startLink;
    Callback stopLink;
    bool requested = false;
    bool windowVisible = true;
    bool running = false;
};
} // namespace eacp::GPU
