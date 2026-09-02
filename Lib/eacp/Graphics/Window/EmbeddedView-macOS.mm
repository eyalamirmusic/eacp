#include "EmbeddedView.h"

#include <eacp/Core/ObjC/ObjC.h>

#import <Cocoa/Cocoa.h>

namespace eacp::Graphics
{

struct EmbeddedView::Native
{
    Native(void* hostParentHandle, const Rect& initialBounds)
    {
        container = [[NSView alloc]
            initWithFrame:NSMakeRect(0, 0, initialBounds.w, initialBounds.h)];

        // Fills the host and follows it as it resizes, until setBounds says
        // otherwise — see the note on EmbeddedView.
        container.get().autoresizingMask =
            NSViewWidthSizable | NSViewHeightSizable;

        [(NSView*) hostParentHandle addSubview:container.get()];

        // After the superview is known, because that is what the frame is
        // measured against and which way up it is measured is its to say.
        setBounds(initialBounds);
    }

    ~Native() { [container.get() removeFromSuperview]; }

    void setContentView(void* contentViewHandle)
    {
        auto* v = (NSView*) contentViewHandle;
        [v setFrame:container.get().bounds];
        [v setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [container.get() addSubview:v];
    }

    void setBounds(const Rect& bounds)
    {
        [container.get() setFrame:toSuperviewFrame(bounds)];
    }

    // Our rect, in the coordinates the superview measures its subviews in.
    //
    // AppKit's default is y-up from the bottom of the superview's bounds, so
    // the conversion depends on the host and not on us: a superview of ours
    // answers YES to isFlipped and is already y-down, and so is a host that
    // flipped its own — but a plain AppKit view is not, and against one of
    // those an unconverted frame puts the surface as far from where it belongs
    // as it is from the bottom of the window.
    NSRect toSuperviewFrame(const Rect& bounds)
    {
        auto* superview = container.get().superview;
        auto y = bounds.y;

        if (superview != nil && !superview.isFlipped)
            y = (float) superview.bounds.size.height - bounds.y - bounds.h;

        return NSMakeRect(bounds.x, y, bounds.w, bounds.h);
    }

    void stopFollowingHost()
    {
        // The mask is ours, not the host's: the constructor set it because a
        // surface nobody places wants to fill its host. Once it is placed,
        // AppKit resizing it behind the host's back is the one thing left that
        // could move it, so the view goes back to plain.
        container.get().autoresizingMask = NSViewNotSizable;
    }

    void setVisible(bool shouldBeVisible)
    {
        container.get().hidden = !shouldBeVisible;
    }

    ObjC::Ptr<NSView> container;
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

// AppKit measures a view in points and does the backing-store arithmetic
// itself, so there is no conversion here for a host to have a different
// opinion about.
void EmbeddedView::setPixelsPerPoint(float) {}

void* EmbeddedView::getHandle()
{
    return impl->container.get();
}

} // namespace eacp::Graphics
