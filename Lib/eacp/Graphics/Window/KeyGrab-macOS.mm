#import <Cocoa/Cocoa.h>

#include "KeyGrab.h"
#include "KeyEchoes.h"
#include "KeyForwarding-macOS.h"

namespace eacp::Graphics
{

struct KeyGrab::Native
{
    Native(Window& windowToUse, Predicate shouldGrabToUse, Handler onGrabbedToUse)
        : window((NSWindow*) windowToUse.getHandle())
        , shouldGrab(std::move(shouldGrabToUse))
        , onGrabbed(std::move(onGrabbedToUse))
    {
        monitor = [NSEvent
            addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp
                                         handler:^NSEvent*(NSEvent* event) {
                                           return intercept(event) ? nil : event;
                                         }];
    }

    ~Native()
    {
        [NSEvent removeMonitor:monitor];
        releaseHeldKeys();
    }

    bool intercept(NSEvent* event)
    {
        auto native = nativeKeyEventFrom(event);

        if (event.window == window && isEchoOfForwardedKey(native))
            return true;

        auto claimed = native.key.type == KeyEventType::Down
                           ? claimDown(event, native)
                           : releaseUp(native);

        if (claimed)
            onGrabbed(native);

        return claimed;
    }

    bool claimDown(NSEvent* event, const NativeKeyEvent& native)
    {
        if (event.window != window || isEditingText() || !shouldGrab(native.key))
            return false;

        heldKeys.addIfNotThere(native.nativeKey);
        return true;
    }

    bool releaseUp(const NativeKeyEvent& native)
    {
        return heldKeys.removeAllMatches(native.nativeKey) > 0;
    }

    bool isEditingText() const
    {
        return [window.firstResponder isKindOfClass:[NSText class]];
    }

    void releaseHeldKeys()
    {
        for (auto keyCode: heldKeys)
            onGrabbed(nativeKeyEventFrom(keyUpFor(keyCode)));

        heldKeys.clear();
    }

    NSEvent* keyUpFor(uint32_t keyCode) const
    {
        return [NSEvent keyEventWithType:NSEventTypeKeyUp
                                location:NSZeroPoint
                           modifierFlags:0
                               timestamp:[NSProcessInfo processInfo].systemUptime
                            windowNumber:window.windowNumber
                                 context:nil
                              characters:@""
             charactersIgnoringModifiers:@""
                               isARepeat:NO
                                 keyCode:static_cast<unsigned short>(keyCode)];
    }

    NSWindow* window;
    Predicate shouldGrab;
    Handler onGrabbed;
    Vector<uint32_t> heldKeys;
    id monitor = nil;
};

KeyGrab::KeyGrab(Window& window, Predicate shouldGrab, Handler onGrabbed)
    : impl(window, std::move(shouldGrab), std::move(onGrabbed))
{
}

KeyGrab::~KeyGrab() = default;

} // namespace eacp::Graphics
