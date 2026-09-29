#import <Cocoa/Cocoa.h>

#include "NativeChildSurface.h"
#include "KeyForwarding-macOS.h"
#include "../Primitives/GraphicUtils.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/RuntimeClass.h>

namespace eacp::Graphics
{

namespace
{
NativeChildSurface*& surfaceOf(id container)
{
    return ObjC::getIvar<NativeChildSurface*>(container, "surface");
}

void offerUnhandledKey(id container, NSEvent* event, KeyEventType type)
{
    auto* surface = surfaceOf(container);
    if (surface != nullptr && surface->onUnhandledKey(nativeKeyEventFrom(event)))
        return;

    auto selector = type == KeyEventType::Down ? @selector(keyDown:) : @selector(keyUp:);
    ObjC::sendSuper<void>(container, [NSView class], selector, event);
}

void containerKeyDown(id self, SEL, NSEvent* event)
{
    offerUnhandledKey(self, event, KeyEventType::Down);
}

void containerKeyUp(id self, SEL, NSEvent* event)
{
    offerUnhandledKey(self, event, KeyEventType::Up);
}

Class getContainerClass()
{
    static auto instance = []
    {
        auto builder = new ObjC::RuntimeClass<NSView>("EacpNativeChildContainer");

        builder->addIvar<NativeChildSurface*>("surface");
        builder->addMethod(@selector(keyDown:), containerKeyDown);
        builder->addMethod(@selector(keyUp:), containerKeyUp);

        builder->registerClass();
        return builder;
    }();

    return instance->get();
}
} // namespace

struct NativeChildSurface::Native
{
    explicit Native(NativeChildSurface& owner)
    {
        auto* surface = (NSView*) owner.getHandle();

        // A stock NSView but for the keys it passes up, deliberately not one of
        // ours. What gets parented in here is written against the view a host
        // hands over, and every host hands over a stock one; ours answers YES
        // to isFlipped, which is a coordinate system the foreign content never
        // agreed to and would lay itself out upside down in.
        container = [[getContainerClass() alloc] initWithFrame:surface.bounds];
        surfaceOf(container.get()) = &owner;

        // Fills the surface and goes on filling it. AppKit applies this from
        // inside setFrame:, synchronously, so the container is the right size
        // the instant the view is resized rather than after the layout pass
        // that would otherwise fix it — which matters to a plugin being told
        // its new size in the same breath.
        container.get().autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        [surface addSubview:container.get()];
    }

    // Takes the container down and nothing else. Whatever was parented into it
    // belongs to another toolkit and is expected to have been detached by now
    // (a VST3 plugin is sent removed() first); a foreign view its own owner
    // still holds a reference to outlives this.
    ~Native()
    {
        surfaceOf(container.get()) = nullptr;
        [container.get() removeFromSuperview];
    }

    void setBounds(const Rect& bounds)
    {
        // No y-axis conversion, unlike EmbeddedView: the superview here is the
        // surface's own view, which is one of ours and therefore flipped —
        // y-down from its top-left, the space Rect is already measured in.
        // EmbeddedView converts because its superview belongs to a host that
        // may well measure the other way up; this one never does.
        [container.get() setFrame:toCGRect(bounds)];
    }

    ObjC::Ptr<NSView> container;
};

NativeChildSurface::NativeChildSurface()
    : impl(*this)
{
}

NativeChildSurface::~NativeChildSurface() = default;

void* NativeChildSurface::getNativeParentHandle()
{
    return impl->container.get();
}

void NativeChildSurface::refreshPlacement()
{
    impl->setBounds(getLocalBounds());
}

void NativeChildSurface::resized()
{
    View::resized();
    refreshPlacement();
}

// Nothing to pass on: the container is a subview of the surface's own view, so
// AppKit carries a hidden ancestor down to it — and down to the foreign
// content under it — without being asked. Windows has no such tree.
void NativeChildSurface::visibilityChanged(bool) {}

} // namespace eacp::Graphics
