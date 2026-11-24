#pragma once

#include <eacp/Core/Utils/Common.h>

namespace eacp::GPU
{
// Visible: render. WindowNotVisible: the window was on screen and is now
// minimised, ordered out, covered or on a sleeping display, so frames would only
// wait on a compositor that is not taking them; the occlusion notification that
// ends it restarts the link. ViewHidden: the view is hidden in its window, so a
// tick skips the frame without stopping the link.
enum class ViewVisibility
{
    Visible,
    WindowNotVisible,
    ViewHidden
};

struct ViewVisibilityFacts
{
    bool headless = false;
    bool hasWindow = false;
    bool viewHidden = false;
    bool windowOnScreen = false;
    bool windowMinimised = false;
    bool windowHasBeenOnScreen = false;
};

// A window that has never been seen on screen - a headless one, one never
// ordered in, one in a session with no window server - renders as if visible:
// it is how frames are rendered and read back there, and it has no occlusion
// change to come that would ever resume it. A view with no window renders too.
ViewVisibility decideViewVisibility(const ViewVisibilityFacts& facts);

// Runs a view's display link only while continuous rendering is asked for and
// the view's window can be seen (see decideViewVisibility).
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
