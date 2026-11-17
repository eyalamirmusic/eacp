#include "Notifications-Apple.h"

#include "../ObjC/Strings.h"
#include "../Utils/Strings.h"

namespace eacp::Notifications::Detail
{
namespace
{
std::optional<std::string> toUserInfoValue(id value)
{
    if ([value isKindOfClass:[NSString class]])
    {
        auto* text = (NSString*) value;
        return Strings::toStdString(text);
    }

    if ([value isKindOfClass:[NSNumber class]])
    {
        auto* number = (NSNumber*) value;
        return Strings::toStdString(number.stringValue);
    }

    return std::nullopt;
}
} // namespace

std::string toDeviceTokenHex(NSData* deviceToken)
{
    const auto* tokenBytes = (const std::uint8_t*) deviceToken.bytes;
    const auto tokenLength = (int) deviceToken.length;

    return Strings::toHex(Span<const std::uint8_t>(tokenBytes, tokenLength));
}

std::map<std::string, std::string> toUserInfoStrings(NSDictionary* userInfo)
{
    auto strings = std::map<std::string, std::string> {};

    for (id key in userInfo)
    {
        if (![key isKindOfClass:[NSString class]])
            continue;

        auto value = toUserInfoValue(userInfo[key]);

        auto* keyText = (NSString*) key;

        if (value)
            strings[Strings::toStdString(keyText)] = *value;
    }

    return strings;
}

ReceivedNotification toReceivedNotification(UNNotificationContent* content)
{
    auto notification = ReceivedNotification {};
    notification.title = Strings::toStdString(content.title);
    notification.body = Strings::toStdString(content.body);
    notification.threadIdentifier = Strings::toStdString(content.threadIdentifier);
    notification.userInfo = toUserInfoStrings(content.userInfo);
    return notification;
}
} // namespace eacp::Notifications::Detail
