#include "EmbeddedView.h"
#import <Cocoa/Cocoa.h>

namespace eacp::Graphics
{

struct EmbeddedView::Native
{
    Native(void* hostParentHandle, const EmbeddedViewOptions& options)
    {
        auto* parent = (NSView*) hostParentHandle;
        auto frame = NSMakeRect(0, 0, options.width, options.height);

        container = [[NSView alloc] initWithFrame:frame];
        container.autoresizingMask =
            NSViewWidthSizable | NSViewHeightSizable;

        [parent addSubview:container];
    }

    ~Native() { [container removeFromSuperview]; }

    void setContentView(void* contentViewHandle)
    {
        auto* v = (NSView*) contentViewHandle;
        [v setFrame:container.bounds];
        [v setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [container addSubview:v];
    }

    void setSize(int width, int height)
    {
        [container setFrameSize:NSMakeSize(width, height)];
    }

    NSView* container = nil;
};

EmbeddedView::EmbeddedView(void* hostParentHandle,
                           const EmbeddedViewOptions& optionsToUse)
    : options(optionsToUse)
    , impl(hostParentHandle, options)
{
}

EmbeddedView::~EmbeddedView() = default;

void EmbeddedView::setContentView(View& view)
{
    impl->setContentView(view.getHandle());
}

void EmbeddedView::setSize(int width, int height)
{
    impl->setSize(width, height);
}

void* EmbeddedView::getHandle()
{
    return impl->container;
}

} // namespace eacp::Graphics
