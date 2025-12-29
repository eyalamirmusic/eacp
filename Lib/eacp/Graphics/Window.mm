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

struct Window::Impl
{
    Impl(const WindowOptions& options)
    {
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                  | NSWindowStyleMaskMiniaturizable;

        if (options.resizable)
            style |= NSWindowStyleMaskResizable;

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
    { [getWindow() setTitle:@(title.c_str())]; }

    NSWindow* getWindow() { return handle.get(); }

    ~Impl() { [handle.get() close]; }

    ObjC::Ptr<NSWindow> handle;
};

Window::Window(const WindowOptions& options)
    : impl(std::make_unique<Impl>(options))
{
}

void Window::setTitle(const std::string& title)
{ impl->setTitle(title); }

void Window::close()
{ impl.reset(); }

} // namespace eacp::Graphics