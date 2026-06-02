#include "Window.h"
#include "../Graphics/Keyboard.h"
#include "../Primitives/GraphicUtils.h"
#include <eacp/Core/App/AppEnvironment.h>
#import <Cocoa/Cocoa.h>

namespace
{
// Reposition the standard window controls to sit `inset` points from the
// window's top-left, preserving the system spacing between them. Mirrors how
// Electron implements trafficLightPosition — there is no NSWindow API for it,
// so we move the buttons directly. Skipped in fullscreen, where macOS owns
// their placement.
void repositionTrafficLights(NSWindow* window, NSPoint inset)
{
    if (window.styleMask & NSWindowStyleMaskFullScreen)
        return;

    NSButton* close = [window standardWindowButton:NSWindowCloseButton];
    NSButton* miniaturize =
        [window standardWindowButton:NSWindowMiniaturizeButton];
    NSButton* zoom = [window standardWindowButton:NSWindowZoomButton];

    if (close == nil || miniaturize == nil || zoom == nil)
        return;

    NSView* container = close.superview;
    CGFloat containerHeight = NSHeight(container.frame);
    CGFloat spacing = NSMinX(miniaturize.frame) - NSMinX(close.frame);

    NSButton* buttons[] = {close, miniaturize, zoom};
    for (int i = 0; i < 3; ++i)
    {
        NSRect frame = buttons[i].frame;
        frame.origin.x = inset.x + i * spacing;
        frame.origin.y = containerHeight - inset.y - NSHeight(frame);
        buttons[i].frame = frame;
    }
}
} // namespace

@interface WindowDelegateBridge : NSObject <NSWindowDelegate>
{
@public
    eacp::Callback cb;
    eacp::Graphics::ResizeCallback onResize;
    eacp::Graphics::WillResizeCallback onWillResize;
    BOOL keepTrafficLightsPositioned;
    NSPoint trafficLightInset;
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
    auto* window = (NSWindow*) notification.object;

    if (keepTrafficLightsPositioned)
        repositionTrafficLights(window, trafficLightInset);

    if (!onResize)
        return;

    auto content = [window contentRectForFrameRect:[window frame]];
    onResize((int) content.size.width, (int) content.size.height);
}
@end

namespace eacp::Graphics
{
WindowDelegateBridge* createWindowDelegate(const WindowOptions& options)
{
    auto bridge = [[WindowDelegateBridge alloc] init];
    bridge->cb = options.effectiveOnQuit();
    bridge->onResize = options.onResize;
    bridge->onWillResize = options.onWillResize;
    bridge->keepTrafficLightsPositioned = options.trafficLightPosition.has_value();

    if (options.trafficLightPosition)
        bridge->trafficLightInset = NSMakePoint(options.trafficLightPosition->x,
                                                options.trafficLightPosition->y);

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
        [getWindow() setTitleVisibility:options.showTitle ? NSWindowTitleVisible
                                                          : NSWindowTitleHidden];
        [getWindow()
            setTitlebarAppearsTransparent:options.titlebarTransparent];

        if (@available(macOS 11.0, *))
        {
            [getWindow() setTitlebarSeparatorStyle:
                             options.showTitlebarSeparator
                                 ? NSTitlebarSeparatorStyleAutomatic
                                 : NSTitlebarSeparatorStyleNone];
        }

        if (options.backgroundColor)
        {
            const auto& c = *options.backgroundColor;
            [getWindow() setBackgroundColor:[NSColor colorWithSRGBRed:c.r
                                                                green:c.g
                                                                 blue:c.b
                                                                alpha:c.a]];
        }

        if (options.minWidth > 0 || options.minHeight > 0)
            [getWindow() setContentMinSize:NSMakeSize(options.minWidth,
                                                      options.minHeight)];

        [getWindow() center];
        [getWindow() setDelegate:delegate.get()];

        // Skip the foreground activation calls under headless mode
        // (CI machines without an active windowing session) — the
        // NSWindow + its content view still exist, so attached
        // WKWebViews load and JS runs; the only thing missing is the
        // visible surface, which tests don't care about.
        if (!eacp::Apps::getAppEnvironment().headless)
        {
            [getWindow() makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }

        if (options.trafficLightPosition)
            repositionTrafficLights(
                getWindow(),
                NSMakePoint(options.trafficLightPosition->x,
                            options.trafficLightPosition->y));
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

Window::~Window() = default;

bool Window::isKeyPressed(uint16_t virtualKeyCode) const
{
    return Keyboard::isKeyPressed(virtualKeyCode);
}

bool Window::isShiftPressed() const
{
    return Keyboard::isShiftPressed();
}

bool Window::isControlPressed() const
{
    return Keyboard::isControlPressed();
}

bool Window::isAltPressed() const
{
    return Keyboard::isAltPressed();
}

bool Window::isCommandPressed() const
{
    return Keyboard::isCommandPressed();
}

ModifierKeys Window::getModifiers() const
{
    return Keyboard::getModifiers();
}

} // namespace eacp::Graphics
