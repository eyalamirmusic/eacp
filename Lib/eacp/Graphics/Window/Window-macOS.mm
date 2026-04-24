#include "Window.h"
#include "../Primitives/GraphicUtils.h"
#import <Cocoa/Cocoa.h>

@interface WindowDelegateBridge : NSObject <NSWindowDelegate>
{
@public
    eacp::Callback cb;
    eacp::Graphics::ResizeCallback onResize;
    eacp::Graphics::WillResizeCallback onWillResize;
}
@end

@implementation WindowDelegateBridge
- (void)windowWillClose:(NSNotification *)notification
{
    cb();
}

- (NSSize)windowWillResize:(NSWindow*)sender toSize:(NSSize)frameSize
{
    if (!onWillResize)
        return frameSize;

    auto proposedFrame = NSMakeRect(0, 0, frameSize.width, frameSize.height);
    auto proposedContent = [sender contentRectForFrameRect:proposedFrame];
    auto width = (int) proposedContent.size.width;
    auto height = (int) proposedContent.size.height;
    onWillResize(width, height);
    proposedContent.size = NSMakeSize(width, height);
    return [sender frameRectForContentRect:proposedContent].size;
}

- (void)windowDidResize:(NSNotification*)notification
{
    if (!onResize)
        return;

    auto* window = (NSWindow*) notification.object;
    auto content = [window contentRectForFrameRect:[window frame]];
    onResize((int) content.size.width, (int) content.size.height);
}
@end

namespace eacp::Graphics
{
WindowDelegateBridge* createWindowDelegate(const WindowOptions& options)
{
    auto bridge = [[WindowDelegateBridge alloc] init];
    bridge->cb = options.onQuit;
    bridge->onResize = options.onResize;
    bridge->onWillResize = options.onWillResize;
    return bridge;
}

NSWindowStyleMask getFlag(WindowFlags flag)
{
    switch (flag)
    {
        case WindowFlags::Borderless:
            return NSWindowStyleMaskBorderless;
        case WindowFlags::Titled:
            return NSWindowStyleMaskTitled;
        case WindowFlags::Closable:
            return NSWindowStyleMaskClosable;
        case WindowFlags::Miniaturizable:
            return NSWindowStyleMaskMiniaturizable;
        case WindowFlags::Resizable:
            return NSWindowStyleMaskResizable;
        case WindowFlags::UnifiedTitleAndToolbar:
            return NSWindowStyleMaskUnifiedTitleAndToolbar;
        case WindowFlags::FullScreen:
            return NSWindowStyleMaskFullScreen;
        case WindowFlags::FullSizeContentView:
            return NSWindowStyleMaskFullSizeContentView;
        case WindowFlags::UtilityWindow:
            return NSWindowStyleMaskUtilityWindow;
        case WindowFlags::DocModalWindow:
            return NSWindowStyleMaskDocModalWindow;
        case WindowFlags::NonactivatingPanel:
            return NSWindowStyleMaskNonactivatingPanel;
        case WindowFlags::HUDWindow:
            return NSWindowStyleMaskHUDWindow;
    }

    return {};
}

NSWindowStyleMask getStyle(const WindowOptions& options)
{
    auto res = NSWindowStyleMask();

    for (auto& flag: options.flags)
        res |= getFlag(flag);

    return res;
}

struct Window::Native
{
    Native(const WindowOptions& options)
    {
        auto style = getStyle(options);
        auto contentRect = NSMakeRect(0, 0, options.width, options.height);

        handle = [[NSWindow alloc] initWithContentRect:contentRect
                                             styleMask:style
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];

        delegate = createWindowDelegate(options);

        [getWindow() setRestorable:NO];
        [getWindow() setReleasedWhenClosed:NO];
        [getWindow() setTitle:@(options.title.c_str())];
        [getWindow() center];
        [getWindow() makeKeyAndOrderFront:nil];
        [getWindow() setDelegate:delegate.get()];

        [NSApp activateIgnoringOtherApps:YES];
    }

    void setTitle(const std::string& title)
    {
        [getWindow() setTitle:@(title.c_str())];
    }

    void setContentView(void* contentView)
    {
        auto v = (NSView*) contentView;
        [getWindow() setContentView:v];
        [v setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    }

    NSWindow* getWindow() { return handle.get(); }

    ~Native() { [handle.get() close]; }

    ObjC::Ptr<NSWindow> handle;
    ObjC::Ptr<WindowDelegateBridge> delegate;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(options)
{
}

void Window::setTitle(const std::string& title)
{
    impl->setTitle(title);
}

void Window::setContentView(View& view)
{
    impl->setContentView(view.getHandle());
}

void* Window::getHandle()
{
    return impl->getWindow();
}

void* Window::getContentViewHandle()
{
    return [impl->getWindow() contentView];
}

Window::~Window()
{
    options.onQuit();
}

} // namespace eacp::Graphics
