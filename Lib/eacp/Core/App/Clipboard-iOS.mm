#import <UIKit/UIKit.h>

#include "Clipboard.h"

#include <eacp/Core/ObjC/Strings.h>

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
} // namespace eacp::Clipboard
