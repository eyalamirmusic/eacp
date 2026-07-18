#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "Common.h"

// The platform half of wheel delivery. Everything above View::dispatchMouseEvent
// is covered portably in ScrollWheelTests.cpp; what cannot be reached from there
// is whether the backing NSView actually implements scrollWheel: at all.
//
// Worth its own test because the failure is silent: an unregistered selector
// means AppKit walks straight past the view up the responder chain, every
// portable test still passes, and the app simply never scrolls. That was the
// state of the framework before this method was added.

using namespace nano;
using namespace eacp::Graphics;

// The backing view implements scrollWheel:, so AppKit delivers wheel events here
// instead of passing them up the responder chain.
auto tNativeViewHandlesScrollWheel = test("ScrollWheel/nativeViewImplementsScrollWheel") = []
{
    auto view = View {};

    auto* native = (__bridge NSView*) view.nativeFocusTarget();
    check(native != nil);

    check([native respondsToSelector:@selector(scrollWheel:)]);

    // respondsToSelector: alone proves nothing here: NSView implements
    // scrollWheel: itself, so it answers YES whether or not eacp registered
    // anything, and inheriting NSView's version is exactly the silent no-op
    // that dropped every wheel event.
    //
    // So compare the resolved method against the one the *immediate superclass*
    // resolves. class_getInstanceMethod walks the chain, so an identical Method
    // pointer means this class added nothing of its own.
    auto* cls = [native class];
    auto* parent = class_getSuperclass(cls);

    check(parent != nil);

    auto* mine = class_getInstanceMethod(cls, @selector(scrollWheel:));
    auto* inherited = class_getInstanceMethod(parent, @selector(scrollWheel:));

    check(mine != nullptr);
    check(mine != inherited);
};

// The sibling mouse selectors are registered on the same class, so the wheel
// method sits alongside them rather than on some other object.
auto tNativeViewHandlesMouseSelectors = test("ScrollWheel/nativeViewImplementsMouseSelectors") = []
{
    auto view = View {};

    auto* native = (__bridge NSView*) view.nativeFocusTarget();
    check(native != nil);

    check([native respondsToSelector:@selector(mouseDown:)]);
    check([native respondsToSelector:@selector(mouseDragged:)]);
    check([native respondsToSelector:@selector(scrollWheel:)]);
};
