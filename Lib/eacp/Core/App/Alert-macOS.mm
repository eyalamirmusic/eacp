#import <AppKit/AppKit.h>

#include "Alert.h"
#include "../ObjC/ObjC.h"
#include "../ObjC/Strings.h"

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

// NSAlert gives its first button Return and a button titled "Cancel" Escape;
// the options say which ones instead.
static void assignKeys(NSAlert* alert, const AlertOptions& options)
{
    auto* buttons = alert.buttons;

    for (NSButton* button in buttons)
        button.keyEquivalent = @"";

    if (options.cancelButton)
        buttons[(NSUInteger) *options.cancelButton].keyEquivalent = @"\033";

    buttons[(NSUInteger) options.defaultButton].keyEquivalent = @"\r";
}

static int pressedButton(NSModalResponse response, const AlertOptions& options)
{
    auto index = (int) (response - NSAlertFirstButtonReturn);

    if (index >= 0 && index < options.buttons.size())
        return index;

    return options.defaultButton;
}

// App-modal through runModal, as JUCE's showMessageBoxAsync is, rather than a
// sheet: a plugin's view may have no window of its own to hang one off.
void Detail::showNativeAlert(const AlertOptions& options, AlertCallback onDismissed)
{
    auto alert = ObjC::Ptr<NSAlert>([[NSAlert alloc] init]);

    alert.get().messageText = alertText(options.title);
    alert.get().informativeText = alertText(options.message);
    alert.get().alertStyle = alertStyleFor(options.icon);

    for (auto& title: options.buttons)
        [alert.get() addButtonWithTitle:alertText(title)];

    assignKeys(alert.get(), options);

    onDismissed(pressedButton([alert.get() runModal], options));
}
} // namespace eacp::Apps
