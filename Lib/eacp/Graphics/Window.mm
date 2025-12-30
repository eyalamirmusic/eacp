#include "Window.h"
#include "../ObjC/ObjC.h"
#import <Cocoa/Cocoa.h>

@interface WindowDelegateBridge : NSObject <NSWindowDelegate>
{
@public
    eacp::Callback cb;
}
@end

@implementation WindowDelegateBridge
- (void)windowWillClose:()notification
{
    cb();
}

WindowDelegateBridge* createWindowDelegate(eacp::Callback cbToUse)
{
    auto bridge = [[WindowDelegateBridge alloc] init];

    if (bridge)
        bridge->cb = cbToUse;

    return bridge;
}

@end

namespace eacp::Graphics
{
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
            return NSWindowToolbarStyleUnified;
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

struct Window::Impl
{
    Impl(const WindowOptions& options)
    {
        auto style = getStyle(options);
        auto contentRect = NSMakeRect(0, 0, options.width, options.height);

        handle = [[NSWindow alloc] initWithContentRect:contentRect
                                             styleMask:style
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];

        [getWindow() setReleasedWhenClosed:NO];
        [getWindow() setTitle:@(options.title.c_str())];
        [getWindow() center];
        [getWindow() makeKeyAndOrderFront:nil];
        [getWindow() setDelegate:createWindowDelegate(options.onQuit)];

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

    ~Impl() { [handle.get() close]; }

    ObjC::Ptr<NSWindow> handle;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(std::make_unique<Impl>(options))
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

Window::~Window()
{
    options.onQuit();
}

} // namespace eacp::Graphics