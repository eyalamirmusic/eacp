#import <AppKit/AppKit.h>

#include "Alert.h"
#include "../ObjC/ObjC.h"
#include "../ObjC/Strings.h"
#include "../Threads/EventLoop-macOS.h"

namespace eacp::Apps
{
static NSAlertStyle alertStyleFor(AlertIcon icon)
{
    switch (icon)
    {
        case AlertIcon::warning:
            return NSAlertStyleWarning;
        case AlertIcon::error:
            return NSAlertStyleCritical;
        case AlertIcon::info:
            break;
    }

    return NSAlertStyleInformational;
}

// Never nil, which addButtonWithTitle: throws on: malformed UTF-8 is blank.
static NSString* alertText(const std::string& text)
{
    auto* string = Strings::toNSString(text);
    return string != nil ? string : @"";
}

static bool escapeNeedsMonitor(const AlertOptions& options)
{
    return options.cancelButton == options.defaultButton;
}

// NSAlert gives its first button Return and a button titled "Cancel" Escape;
// the options say which ones instead. A button has one key equivalent, so a
// cancel button that is also the default keeps Return, and Escape reaches it
// through watchEscape.
static void assignKeys(NSAlert* alert, const AlertOptions& options)
{
    auto* buttons = alert.buttons;

    for (NSButton* button in buttons)
        button.keyEquivalent = @"";

    if (options.cancelButton && !escapeNeedsMonitor(options))
        buttons[(NSUInteger) *options.cancelButton].keyEquivalent = @"\033";

    buttons[(NSUInteger) options.defaultButton].keyEquivalent = @"\r";
}

static id watchEscape(NSAlert* alert, const AlertOptions& options)
{
    if (!options.cancelButton || !escapeNeedsMonitor(options))
        return nil;

    auto response = NSAlertFirstButtonReturn + *options.cancelButton;
    constexpr auto escapeKeyCode = 53;

    return [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                     handler:^NSEvent*(NSEvent* event) {
                                       if (event.keyCode != escapeKeyCode
                                           || event.window != alert.window)
                                           return event;

                                       [NSApp stopModalWithCode:response];
                                       return nil;
                                     }];
}

// runModal, as JUCE's showMessageBoxAsync is, rather than a sheet: a plugin's
// view may have no window of its own to hang one off. A quit that arrives while
// it is up ends the modal session instead of the loop, so runModal answers
// NSModalResponseStop: that is no button, so unanswered, and the quit is put to
// the loop again.
void Detail::showNativeAlert(const AlertOptions& options, AlertCallback onDismissed)
{
    auto alert = ObjC::Ptr<NSAlert>([[NSAlert alloc] init]);

    alert.get().messageText = alertText(options.title);
    alert.get().informativeText = alertText(options.message);
    alert.get().alertStyle = alertStyleFor(options.icon);

    for (auto& title: options.buttons)
        [alert.get() addButtonWithTitle:alertText(title)];

    assignKeys(alert.get(), options);

    id escapeMonitor = watchEscape(alert.get(), options);
    auto response = [alert.get() runModal];

    if (escapeMonitor != nil)
        [NSEvent removeMonitor:escapeMonitor];

    if (response == NSModalResponseStop)
        Threads::Detail::resumeQuitAfterModal();

    onDismissed(answerForButton(options, (long) (response - NSAlertFirstButtonReturn)));
}
} // namespace eacp::Apps
