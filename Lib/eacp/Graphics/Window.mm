#include "Window.h"
#include "../ObjC/ObjC.h"
#include "../Utils/Common.h"
#import <Cocoa/Cocoa.h>

@interface WindowDelegateBridge : NSObject <NSWindowDelegate>
{
    eacp::Callback onCloseCallback;
}

- (instancetype)initWithCallback:(eacp::Callback)callback;
@end

@implementation WindowDelegateBridge

- (instancetype)initWithCallback:(eacp::Callback)callback
{
    self = [super init];

    if (self)
    {
        onCloseCallback = callback;
    }

    return self;
}

// This method is called automatically by macOS when 'X' is clicked
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    if (onCloseCallback)
    {
        onCloseCallback();
    }

    return YES;
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

        [NSApp activateIgnoringOtherApps:YES];
    }

    void setTitle(const std::string& title)
    {
        [getWindow() setTitle:@(title.c_str())];
    }

    NSWindow* getWindow() { return handle.get(); }

    ~Impl() { [handle.get() close]; }

    ObjC::Ptr<NSWindow> handle;
};

Window::Window(const WindowOptions& options)
    : impl(std::make_unique<Impl>(options))
{
}

void Window::setTitle(const std::string& title)
{
    impl->setTitle(title);
}

void Window::close()
{
    impl.reset();
}

} // namespace eacp::Graphics