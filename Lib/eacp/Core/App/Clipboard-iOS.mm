#import <UIKit/UIKit.h>

#include "Clipboard.h"

#include "../ObjC/Strings.h"

namespace eacp::Clipboard
{
bool copyText(std::string_view text)
{
    auto storage = std::string {text};
    [UIPasteboard generalPasteboard].string = Strings::toNSString(storage);
    return true;
}

std::string getText()
{
    auto* text = [UIPasteboard generalPasteboard].string;

    if (text == nil)
        return {};

    return Strings::toStdString(text);
}

bool hasText()
{
    // UIPasteboard answers this without materialising the string, which on iOS
    // also avoids the paste-notification banner a read would trigger.
    return [UIPasteboard generalPasteboard].hasStrings == YES;
}

bool copyFiles(const Vector<std::string>&)
{
    return false;
}
} // namespace eacp::Clipboard
