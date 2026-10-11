#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

auto tNativeViewHandlesMagnify = test("Magnify/nativeViewImplementsMagnifyWithEvent") = []
{
    auto view = View {};

    auto* native = (__bridge NSView*) view.nativeFocusTarget();
    check(native != nil);

    auto* cls = [native class];
    auto* parent = class_getSuperclass(cls);
    check(parent != nil);

    auto* mine = class_getInstanceMethod(cls, @selector(magnifyWithEvent:));
    auto* inherited = class_getInstanceMethod(parent, @selector(magnifyWithEvent:));

    check(mine != nullptr);
    check(mine != inherited);
};
