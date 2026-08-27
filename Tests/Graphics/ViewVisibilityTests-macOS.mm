#import <AppKit/AppKit.h>

#include "Common.h"

// The platform half of View::setVisible. ViewVisibilityTests.cpp covers the
// bookkeeping; what cannot be reached from there is whether AppKit was told.
//
// The second test is the one the feature rests on. Hiding a view is only worth
// anything to an embedded web view if AppKit carries it down to the nested
// native surface — that is what sends WKWebView viewDidHide, which is what
// takes its page out of the visible state and lets WebKit reclaim it. Nothing
// in portable code can see that happen, and if it did not, every portable test
// would still pass while the page stayed live and expensive.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
NSView* nativeViewOf(View& view)
{
    return (__bridge NSView*) view.getHandle();
}
} // namespace

auto tSetVisibleReachesAppKit = test("ViewVisibility/setVisibleReachesAppKit") = []
{
    auto view = View {};

    check(!nativeViewOf(view).hidden);

    view.setVisible(false);
    check(nativeViewOf(view).hidden);

    view.setVisible(true);
    check(!nativeViewOf(view).hidden);
};

// A hidden ancestor hides the native view nested under it, without that view's
// own hidden flag ever being set. This is the propagation a WebView depends on.
auto tHiddenAncestorHidesNestedNativeView =
    test("ViewVisibility/hiddenAncestorHidesNestedNativeView") = []
{
    auto parent = View {};
    auto child = View {};

    parent.addSubview(child);

    check(!nativeViewOf(child).isHiddenOrHasHiddenAncestor);

    parent.setVisible(false);

    check(nativeViewOf(child).isHiddenOrHasHiddenAncestor);

    // The child was never hidden in its own right — only by where it sits.
    check(!nativeViewOf(child).hidden);

    parent.setVisible(true);
    check(!nativeViewOf(child).isHiddenOrHasHiddenAncestor);
};
