#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

auto tNativeViewHandlesScrollWheel = test("ScrollWheel/nativeViewImplementsScrollWheel") = []
{
    auto view = View {};

    auto* native = (__bridge NSView*) view.nativeFocusTarget();
    check(native != nil);

    check([native respondsToSelector:@selector(scrollWheel:)]);

    // NSView implements scrollWheel: itself, so respondsToSelector: answers YES
    // either way; an identical Method pointer means this class added nothing.
    auto* cls = [native class];
    auto* parent = class_getSuperclass(cls);

    check(parent != nil);

    auto* mine = class_getInstanceMethod(cls, @selector(scrollWheel:));
    auto* inherited = class_getInstanceMethod(parent, @selector(scrollWheel:));

    check(mine != nullptr);
    check(mine != inherited);
};

auto tNativeViewHandlesMouseSelectors = test("ScrollWheel/nativeViewImplementsMouseSelectors") = []
{
    auto view = View {};

    auto* native = (__bridge NSView*) view.nativeFocusTarget();
    check(native != nil);

    check([native respondsToSelector:@selector(mouseDown:)]);
    check([native respondsToSelector:@selector(mouseDragged:)]);
    check([native respondsToSelector:@selector(scrollWheel:)]);
};
