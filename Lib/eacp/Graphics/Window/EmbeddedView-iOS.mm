#include "EmbeddedView.h"
#include "../Primitives/GraphicUtils.h"

#include <eacp/Core/ObjC/ObjC.h>

#import <UIKit/UIKit.h>

// The surface a host's UIView takes, which on iOS is mostly a host handing over
// the view an AUViewController is given: an app extension's process, where
// UIApplication is not ours to ask for and there is no window or scene of ours
// to find. So nothing here reaches past the view it was handed — no
// UIApplication, no scene, no screen beyond the one the content's own views
// read once they are in a window (View-iOS.mm's didMoveToWindow) — and
// Window-iOS.mm, which does all three, is never on this path.
//
// Everything else is UIKit's: the content View is a UIView like any other, it
// draws and presents (a GPUView's CAMetalLayer included) exactly as it does in a
// window of ours, and it takes touches the same way, because the touch path
// looks for the outermost eacp view above the one touched and that is the
// content view, whatever the host has around it.

namespace eacp::Graphics
{

struct EmbeddedView::Native
{
    Native(void* hostParentHandle, const Rect& initialBounds)
    {
        container = [[UIView alloc] initWithFrame:toCGRect(initialBounds)];

        // A host's own paint shows through until ours lands, as on the other
        // platforms.
        container.get().opaque = NO;
        container.get().backgroundColor = UIColor.clearColor;

        // Fills the host and follows it as it resizes, until setBounds says
        // otherwise — see the note on EmbeddedView.
        container.get().autoresizingMask =
            UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

        [(__bridge UIView*) hostParentHandle addSubview:container.get()];
    }

    ~Native() { [container.get() removeFromSuperview]; }

    void setContentView(void* contentViewHandle)
    {
        auto* v = (__bridge UIView*) contentViewHandle;
        v.frame = container.get().bounds;
        v.autoresizingMask =
            UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        [container.get() addSubview:v];
    }

    // UIKit measures every view y-down from its superview's top-left, which is
    // eacp's own space, so unlike AppKit there is no host convention to convert
    // from: the frame is the bounds.
    void setBounds(const Rect& bounds) { container.get().frame = toCGRect(bounds); }

    void stopFollowingHost()
    {
        // The mask is ours, not the host's — the same reasoning as on macOS.
        container.get().autoresizingMask = UIViewAutoresizingNone;
    }

    void setVisible(bool shouldBeVisible)
    {
        container.get().hidden = !shouldBeVisible;
    }

    ObjC::Ptr<UIView> container;
};

EmbeddedView::EmbeddedView(void* hostParentHandle,
                           const EmbeddedViewOptions& optionsToUse)
    : bounds(0.f, 0.f, (float) optionsToUse.width, (float) optionsToUse.height)
    , impl(hostParentHandle, bounds)
{
}

EmbeddedView::~EmbeddedView() = default;

void EmbeddedView::setContentView(View& view)
{
    impl->setContentView(view.getHandle());
}

void EmbeddedView::setBounds(const Rect& newBounds)
{
    bounds = newBounds;

    impl->stopFollowingHost();
    impl->setBounds(bounds);
}

void EmbeddedView::setSize(int width, int height)
{
    bounds = bounds.withSize((float) width, (float) height);
    impl->setBounds(bounds);
}

void EmbeddedView::setVisible(bool shouldBeVisible)
{
    visible = shouldBeVisible;
    impl->setVisible(shouldBeVisible);
}

// UIKit measures a view in points and the content's views take their scale
// from the screen of the window they land in (View-iOS.mm), so as on macOS
// there is no conversion here for a host to have a different opinion about.
void EmbeddedView::setPixelsPerPoint(float) {}

void* EmbeddedView::getHandle()
{
    return (__bridge void*) impl->container.get();
}

} // namespace eacp::Graphics
