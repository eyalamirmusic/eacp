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

bool copyFiles(const Vector<std::string>&)
{
    return false;
}

std::string getText()
{
    return Strings::toStdString([UIPasteboard generalPasteboard].string);
}
} // namespace eacp::Clipboard
