#import <AppKit/AppKit.h>

#include "Common.h"

#include <eacp/Core/ObjC/ObjC.h>

// Where an embedded surface lands inside the host's view, which is the one
// thing about embedding that nothing portable can check and nothing else
// fails over.
//
// A host hands over an NSView and says where the surface goes in eacp's own
// space: points, y-down from the top-left. AppKit measures a subview's frame
// the other way up unless the superview says otherwise, and the superview is
// the host's, so which way up it is measured is not ours to assume. Get the
// conversion wrong and the surface is placed as far from where it belongs as
// it is from the bottom of the host — a whole window out, in the one case
// (a host with a layout of its own) the placement API exists for, and dead on
// in the case that made the code look right (a surface filling its host).

using namespace nano;
using namespace eacp::Graphics;

@interface FlippedHostView : NSView
@end

@implementation FlippedHostView
- (BOOL) isFlipped
{
    return YES;
}
@end

namespace
{
constexpr auto hostWidth = 400.f;
constexpr auto hostHeight = 300.f;

template <typename HostClass>
eacp::ObjC::Ptr<NSView> makeHost()
{
    auto frame = NSMakeRect(0, 0, hostWidth, hostHeight);
    return {[[HostClass alloc] initWithFrame:frame]};
}

NSView* surfaceOf(EmbeddedView& embedded)
{
    return (__bridge NSView*) embedded.getHandle();
}

bool same(double a, double b)
{
    return std::abs(a - b) < 0.0001;
}
} // namespace

// The host is a plain AppKit view, so its subviews are measured up from its
// bottom edge: y = 40 from the top of a 300-point host is 200 in its own
// coordinates, and the surface is 60 tall.
auto tPlacesFromTheTopOfAnUnflippedHost =
    test("EmbeddedView/placesFromTheTopOfAnUnflippedHost") = []
{
    auto host = makeHost<NSView>();
    auto embedded = EmbeddedView {host.get()};

    embedded.setBounds({20.f, 40.f, 100.f, 60.f});

    auto frame = surfaceOf(embedded).frame;

    check(same(frame.origin.x, 20.));
    check(same(frame.origin.y, hostHeight - 40. - 60.));
    check(same(frame.size.width, 100.));
    check(same(frame.size.height, 60.));

    // And the rect it was given is the rect it reports, whatever AppKit was
    // told underneath.
    check(same(embedded.getBounds().y, 40.));
};

// A host that has flipped its own view — every view of ours is one — is
// already measuring the way eacp does, and then the frame is the bounds.
auto tPlacesDirectlyInsideAFlippedHost =
    test("EmbeddedView/placesDirectlyInsideAFlippedHost") = []
{
    auto host = makeHost<FlippedHostView>();
    auto embedded = EmbeddedView {host.get()};

    embedded.setBounds({20.f, 40.f, 100.f, 60.f});

    auto frame = surfaceOf(embedded).frame;

    check(same(frame.origin.x, 20.));
    check(same(frame.origin.y, 40.));
};

// A surface nobody places fills its host and follows it, which is the whole of
// what a host with no layout has to do. Placing one says the host has a layout
// after all, and AppKit resizing the surface behind its back is then the one
// thing left that could move it.
auto tPlacingTakesTheSurfaceOffAutomaticSizing =
    test("EmbeddedView/placingTakesTheSurfaceOffAutomaticSizing") = []
{
    auto host = makeHost<NSView>();
    auto embedded = EmbeddedView {host.get()};

    check(surfaceOf(embedded).autoresizingMask
          == (NSViewWidthSizable | NSViewHeightSizable));

    embedded.setBounds({0.f, 0.f, 100.f, 60.f});

    check(surfaceOf(embedded).autoresizingMask == NSViewNotSizable);
};

// setSize is the other host's call — the one that handed over its whole window
// and is passing on a resize — so it leaves that automatic sizing alone.
auto tResizingLeavesAutomaticSizingAlone =
    test("EmbeddedView/resizingLeavesAutomaticSizingAlone") = []
{
    auto host = makeHost<NSView>();
    auto embedded = EmbeddedView {host.get()};

    embedded.setSize(320, 200);

    check(surfaceOf(embedded).autoresizingMask
          == (NSViewWidthSizable | NSViewHeightSizable));

    // Resized about its top-left, which in an unflipped host means the frame's
    // origin moves by the difference in height.
    check(same(surfaceOf(embedded).frame.origin.y, hostHeight - 200.));
    check(same(embedded.getBounds().w, 320.));
};

auto tVisibilityReachesAppKit =
    test("EmbeddedView/visibilityReachesAppKit") = []
{
    auto host = makeHost<NSView>();
    auto embedded = EmbeddedView {host.get()};

    check(embedded.isVisible());
    check(!surfaceOf(embedded).hidden);

    embedded.setVisible(false);

    check(!embedded.isVisible());
    check(surfaceOf(embedded).hidden);

    embedded.setVisible(true);

    check(embedded.isVisible());
    check(!surfaceOf(embedded).hidden);
};

// The surface is the host's subview for exactly as long as the EmbeddedView
// exists: a host that closes and reopens its window builds a new one against
// the new handle, and the old one has to be gone from the old window by then.
auto tSurfaceLeavesTheHostWithTheEmbeddedView =
    test("EmbeddedView/surfaceLeavesTheHostWithTheEmbeddedView") = []
{
    auto host = makeHost<NSView>();

    {
        auto embedded = EmbeddedView {host.get()};
        check(host.get().subviews.count == 1);
    }

    check(host.get().subviews.count == 0);
};
