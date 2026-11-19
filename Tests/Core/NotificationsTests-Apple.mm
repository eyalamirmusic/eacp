#include "Common.h"

#include <eacp/Core/Notifications/Notifications-Apple.h>

using namespace nano;

namespace NotificationsAppleTests
{
namespace Detail = eacp::Notifications::Detail;

auto tTokenHex = test("Notifications/aDeviceTokenIsWrittenAsLowercaseHex") = []
{
    constexpr auto tokenBytes = std::array<std::uint8_t, 4> {0x0a, 0xff, 0x00, 0x7c};

    auto* token = [NSData dataWithBytes:tokenBytes.data() length:tokenBytes.size()];

    check(Detail::toDeviceTokenHex(token) == "0aff007c");
};

auto tUserInfo =
    test("Notifications/userInfoKeepsStringsAndNumbersAndLeavesObjectsOut") = []
{
    auto* userInfo = @{
        @"pane": @"76365028b7cad21f",
        @"attempt": @42,
        @"isQuestion": @YES,
        @"aps": @{@"sound": @"default"},
        @"hosts": @[@"tamby"],
    };

    auto strings = Detail::toUserInfoStrings(userInfo);

    check(strings.size() == 3);
    check(strings.at("pane") == "76365028b7cad21f");
    check(strings.at("attempt") == "42");
    check(strings.at("isQuestion") == "1");
    check(!strings.contains("aps"));
    check(!strings.contains("hosts"));
};

auto tContent =
    test("Notifications/aReceivedNotificationCarriesTitleBodyThreadAndUserInfo") = []
{
    auto* content = [[[UNMutableNotificationContent alloc] init] autorelease];
    content.title = @"Claude finished";
    content.body = @"tamby: CowTerm";
    content.threadIdentifier = @"tamby/CowTerm";
    content.userInfo = @{@"pane": @"76365028b7cad21f"};

    auto notification = Detail::toReceivedNotification(content);

    check(notification.title == "Claude finished");
    check(notification.body == "tamby: CowTerm");
    check(notification.threadIdentifier == "tamby/CowTerm");
    check(notification.userInfo.at("pane") == "76365028b7cad21f");
};
} // namespace NotificationsAppleTests
