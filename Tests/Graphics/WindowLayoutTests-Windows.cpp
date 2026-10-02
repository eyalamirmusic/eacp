#include "Common.h"

#include <eacp/Core/Threads/EventLoop.h>

// A content view is sized the moment it is attached, which is usually inside
// the constructor of the app that owns the window - before that app has added
// the subviews its resized() lays out. Nothing laid the tree out again until a
// WM_SIZE, which a window shown at its created size never gets, so those
// subviews kept zero bounds: Cows In Love opened to a grey window. The host now
// posts itself a layout pass, which runs once the message loop does.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
struct Host final : View
{
    void resized() override { child.scaleToFit(); }

    View child;
};
} // namespace

auto tLaysOutSubviewsAddedAfterAttaching =
    test("Window/laysOutSubviewsAddedAfterAttaching") = []
{
    auto window = Window {};
    auto content = Host {};
    window.setContentView(content);
    content.addSubview(content.child);

    check(!content.getLocalBounds().isEmpty());
    check(content.child.getBounds().isEmpty());

    auto laidOut = [&] { return !content.child.getBounds().isEmpty(); };
    check(Threads::runEventLoopUntil(laidOut, Time::MS {1000}));

    auto bounds = content.child.getBounds();
    check(bounds.w == content.getLocalBounds().w);
    check(bounds.h == content.getLocalBounds().h);
};
