#import <UIKit/UIKit.h>

#include "Common.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <cmath>

// The UIKit half of embedding, which is what an AUv3's AUViewController hands
// an editor: a UIView of the host's, with nothing of ours around it — no
// Window, no scene, no UIApplication an app extension may ask for.
//
// UIKit measures y-down, so unlike AppKit there is no conversion for a host to
// get wrong; what is left to check is that the surface lands where it is put,
// fills and follows the host until it is put somewhere, carries its content
// with it, takes the touches over its rectangle to the content, picks up the
// scale of the screen it actually lands on, and is gone from the host when the
// EmbeddedView is.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
constexpr auto hostWidth = 400.f;
constexpr auto hostHeight = 300.f;

eacp::ObjC::Ptr<UIView> makeHost()
{
    auto frame = CGRectMake(0, 0, hostWidth, hostHeight);
    return {[[UIView alloc] initWithFrame:frame]};
}

UIView* surfaceOf(EmbeddedView& embedded)
{
    return (__bridge UIView*) embedded.getHandle();
}

UIView* nativeViewOf(View& view)
{
    return (__bridge UIView*) view.getHandle();
}

constexpr auto followsHost =
    UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

struct ScaleCountingView final : View
{
    void backingScaleChanged() override { ++changes; }

    int changes = 0;
};
} // namespace

// The rect is in eacp's space and UIKit's is the same one, so the frame is the
// bounds, measured from the top of the host.
auto tPlacesFromTheTopOfTheHost = test("EmbeddedView/placesFromTheTopOfTheHost") = []
{
    auto host = makeHost();
    auto embedded = EmbeddedView {host.get()};

    embedded.setBounds({20.f, 40.f, 100.f, 60.f});

    auto frame = surfaceOf(embedded).frame;

    check(same(frame.origin.x, 20.));
    check(same(frame.origin.y, 40.));
    check(same(frame.size.width, 100.));
    check(same(frame.size.height, 60.));
    check(same(embedded.getBounds().y, 40.));
};

auto tIsTheHostsSubview = test("EmbeddedView/isTheHostsSubview") = []
{
    auto host = makeHost();
    auto embedded = EmbeddedView {host.get()};

    check(surfaceOf(embedded).superview == host.get());
};

// A surface nobody places fills its host and follows it; placing one takes it
// off that for good, as on the other platforms.
auto tPlacingTakesTheSurfaceOffAutomaticSizing =
    test("EmbeddedView/placingTakesTheSurfaceOffAutomaticSizing") = []
{
    auto host = makeHost();
    auto embedded = EmbeddedView {host.get()};

    check(surfaceOf(embedded).autoresizingMask == followsHost);

    embedded.setBounds({0.f, 0.f, 100.f, 60.f});

    check(surfaceOf(embedded).autoresizingMask == UIViewAutoresizingNone);
};

auto tResizingLeavesAutomaticSizingAlone =
    test("EmbeddedView/resizingLeavesAutomaticSizingAlone") = []
{
    auto host = makeHost();
    auto embedded = EmbeddedView {host.get()};

    embedded.setSize(320, 200);

    check(surfaceOf(embedded).autoresizingMask == followsHost);
    check(same(surfaceOf(embedded).frame.origin.y, 0.));
    check(same(surfaceOf(embedded).frame.size.width, 320.));
    check(same(embedded.getBounds().h, 200.));
};

// The content fills the surface and keeps filling it when the host resizes
// the surface — the AUv3 case, where the host sizes the view controller's view
// and nobody calls setSize at all — and its View sees the size in points.
auto tContentFollowsTheHostsSize = test("EmbeddedView/contentFollowsTheHostsSize") = []
{
    auto host = makeHost();
    auto embedded = EmbeddedView {host.get(), {(int) hostWidth, (int) hostHeight}};
    auto content = View {};

    embedded.setContentView(content);

    check(nativeViewOf(content).superview == surfaceOf(embedded));
    check(same(content.getBounds().w, hostWidth));
    check(same(content.getBounds().h, hostHeight));

    host.get().frame = CGRectMake(0, 0, 500, 360);
    [host.get() layoutIfNeeded];

    check(same(surfaceOf(embedded).frame.size.width, 500.));
    check(same(content.getBounds().w, 500.));
    check(same(content.getBounds().h, 360.));
};

// A touch inside the surface's rectangle reaches the content's own UIView,
// which is where View-iOS.mm turns it into a TouchEvent: the content is the
// outermost eacp view there, so it is the root the event is dispatched from.
auto tTouchesOverTheSurfaceReachTheContent =
    test("EmbeddedView/touchesOverTheSurfaceReachTheContent") = []
{
    auto host = makeHost();
    auto embedded = EmbeddedView {host.get()};
    auto content = View {};

    embedded.setContentView(content);
    embedded.setBounds({50.f, 50.f, 100.f, 100.f});

    check([host.get() hitTest:CGPointMake(100, 100) withEvent:nil]
          == nativeViewOf(content));
    check([host.get() hitTest:CGPointMake(10, 10) withEvent:nil] == host.get());
};

auto tVisibilityReachesUIKit = test("EmbeddedView/visibilityReachesUIKit") = []
{
    auto host = makeHost();
    auto embedded = EmbeddedView {host.get()};

    check(!surfaceOf(embedded).hidden);

    embedded.setVisible(false);

    check(!embedded.isVisible());
    check(surfaceOf(embedded).hidden);

    embedded.setVisible(true);

    check(!surfaceOf(embedded).hidden);
};

// The content takes the scale of the screen it lands on when it lands, not the
// one it guessed at creation, and says so to the C++ side so pixel-sized
// resources (a GPUView's drawable) follow. Seeded at 1 here so the landing is
// a change on any screen the simulator has.
auto tContentTakesTheScaleOfTheScreenItLandsOn =
    test("EmbeddedView/contentTakesTheScaleOfTheScreenItLandsOn") = []
{
    auto window = eacp::ObjC::Ptr<UIWindow> {
        [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, hostWidth, hostHeight)]};
    auto host = makeHost();
    auto embedded = EmbeddedView {host.get()};
    auto content = ScaleCountingView {};

    embedded.setContentView(content);
    nativeViewOf(content).contentScaleFactor = 1.0;

    [window.get() addSubview:host.get()];

    auto screenScale = window.get().traitCollection.displayScale;
    check(screenScale > 0.0);

    // A 1x screen would make the landing no change at all; the simulator's
    // devices are all 2x or 3x.
    if (screenScale != 1.0)
    {
        check(same(nativeViewOf(content).contentScaleFactor, screenScale));
        check(same(nativeViewOf(content).layer.contentsScale, screenScale));
        check(content.changes == 1);
    }

    [host.get() removeFromSuperview];
};

auto tSurfaceLeavesTheHostWithTheEmbeddedView =
    test("EmbeddedView/surfaceLeavesTheHostWithTheEmbeddedView") = []
{
    auto host = makeHost();

    {
        auto embedded = EmbeddedView {host.get()};
        check(host.get().subviews.count == 1);
    }

    check(host.get().subviews.count == 0);
};
