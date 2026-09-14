#import <AppKit/AppKit.h>

#include "Common.h"

#include <eacp/Core/ObjC/ObjC.h>

// Where foreign content lands when one of our layouts makes room for it —
// NativeChildSurface, the direction EmbeddedView does not go.
//
// Nothing portable can check any of this. The whole point of the class is the
// native handle it hands a toolkit that is not ours, and the only way to know
// the handle is in the right place is to ask AppKit where it put it. Get it
// wrong and a hosted plugin's editor still draws, still takes the mouse, and
// is somewhere else in the window — which is exactly the failure EmbeddedView's
// y-axis bug was, from the other side.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
NSView* nativeViewOf(View& view)
{
    return (__bridge NSView*) view.getHandle();
}

// The handle a plugin would be given: what its own view becomes a subview of.
NSView* foreignParentOf(NativeChildSurface& surface)
{
    return (__bridge NSView*) surface.getNativeParentHandle();
}

bool same(double a, double b)
{
    return std::abs(a - b) < 0.0001;
}
} // namespace

// Two handles, two jobs. View::getHandle() is the view eacp draws through;
// this one is a stock AppKit view inside it, and stock is the point — foreign
// content is written against the unflipped view every host hands over, and
// ours is flipped.
auto tOffersAStockViewInsideItsOwn =
    test("NativeChildSurface/offersAStockViewInsideItsOwn") = []
{
    auto surface = NativeChildSurface {};
    auto* foreignParent = foreignParentOf(surface);

    check(foreignParent != nil);
    check(foreignParent != nativeViewOf(surface));
    check(foreignParent.superview == nativeViewOf(surface));
    check(!foreignParent.isFlipped);
};

// The one that matters: a surface placed by a layout puts the foreign content
// over the rect the layout asked for, in a real window.
auto tCoversTheBoundsItWasGiven =
    test("NativeChildSurface/coversTheBoundsItWasGiven") = []
{
    auto content = View {};
    auto surface = NativeChildSurface {};
    content.addSubview(surface);

    auto options = WindowOptions {};
    options.width = 400;
    options.height = 300;

    auto window = Window {content, options};

    surface.setBounds({20.f, 40.f, 100.f, 60.f});

    // Both the paths that place the container, agreeing: setBounds resized the
    // surface and AppKit's autoresizing carried it straight through, and this
    // is the explicit placement the next layout pass would make anyway.
    surface.refreshPlacement();

    check(surface.getWindow() == &window);

    // It fills the surface, from the surface's own top-left...
    auto* foreignParent = foreignParentOf(surface);
    auto frame = foreignParent.frame;

    check(same(frame.origin.x, 0.));
    check(same(frame.origin.y, 0.));
    check(same(frame.size.width, 100.));
    check(same(frame.size.height, 60.));

    // ...and the surface is where it was put, so the foreign content is too.
    // Converted back into the content view's space — one of ours, so y-down
    // from its top-left — the rect is the one the layout asked for, which is
    // the whole claim the class makes.
    auto placed = [foreignParent convertRect:foreignParent.bounds
                                      toView:nativeViewOf(content)];

    check(same(placed.origin.x, 20.));
    check(same(placed.origin.y, 40.));
    check(same(placed.size.width, 100.));
    check(same(placed.size.height, 60.));
};

// Without waiting for a layout pass. A plugin is told its new size in the same
// breath as the resize (IPlugView::onSize from resized()), and a container
// still at the old size when that happens draws the plugin clipped until
// AppKit next lays out.
auto tFollowsAResizeImmediately =
    test("NativeChildSurface/followsAResizeImmediately") = []
{
    auto surface = NativeChildSurface {};
    auto* foreignParent = foreignParentOf(surface);

    surface.setBounds({0.f, 0.f, 320.f, 200.f});
    check(same(foreignParent.frame.size.width, 320.));
    check(same(foreignParent.frame.size.height, 200.));

    surface.setBounds({10.f, 10.f, 640.f, 480.f});
    check(same(foreignParent.frame.size.width, 640.));
    check(same(foreignParent.frame.size.height, 480.));
};

// Foreign toolkits set their own parent's frame — plugins do it routinely, and
// a VST3 one does it from inside attached(). refreshPlacement is how the
// surface takes the rectangle back.
auto tRefreshPlacementPutsTheContainerBack =
    test("NativeChildSurface/refreshPlacementPutsTheContainerBack") = []
{
    auto surface = NativeChildSurface {};
    surface.setBounds({0.f, 0.f, 320.f, 200.f});

    auto* foreignParent = foreignParentOf(surface);
    [foreignParent setFrame:NSMakeRect(7.f, 9.f, 11.f, 13.f)];

    surface.refreshPlacement();

    check(same(foreignParent.frame.origin.x, 0.));
    check(same(foreignParent.frame.origin.y, 0.));
    check(same(foreignParent.frame.size.width, 320.));
    check(same(foreignParent.frame.size.height, 200.));
};

// Hiding the surface has to reach the foreign content, or a hidden editor goes
// on drawing over the layout that hid it. AppKit carries it down the subview
// chain for free; Windows will not, which is why the hook is in the header.
auto tHidingReachesTheForeignContent =
    test("NativeChildSurface/hidingReachesTheForeignContent") = []
{
    auto surface = NativeChildSurface {};
    auto* foreignParent = foreignParentOf(surface);

    check(!foreignParent.isHiddenOrHasHiddenAncestor);

    surface.setVisible(false);
    check(foreignParent.isHiddenOrHasHiddenAncestor);

    surface.setVisible(true);
    check(!foreignParent.isHiddenOrHasHiddenAncestor);
};

// The container is in the view hierarchy for exactly as long as the surface
// is: a host that closes and reopens an editor builds a new surface, and the
// old container must be gone from the old window by then.
auto tContainerLeavesWithTheSurface =
    test("NativeChildSurface/containerLeavesWithTheSurface") = []
{
    auto parent = View {};
    auto held = eacp::ObjC::Ptr<NSView> {};

    {
        auto surface = NativeChildSurface {};
        parent.addSubview(surface);

        held = eacp::ObjC::attachPtr(foreignParentOf(surface));
        check(held.get().superview != nil);
    }

    check(held.get().superview == nil);
};

// And the content is never ours to free. A plugin keeps its own reference to
// the view it built; destroying the surface out from under one that was still
// attached must not take it with us — reading the frame off a freed view is
// what this would catch.
auto tForeignContentOutlivesTheSurface =
    test("NativeChildSurface/foreignContentOutlivesTheSurface") = []
{
    auto frame = NSMakeRect(0, 0, 10, 10);
    auto foreign = eacp::ObjC::Ptr<NSView> {[[NSView alloc] initWithFrame:frame]};

    {
        auto surface = NativeChildSurface {};
        [foreignParentOf(surface) addSubview:foreign.get()];
    }

    check(same(foreign.get().frame.size.width, 10.));
    check(same(foreign.get().frame.size.height, 10.));
};
