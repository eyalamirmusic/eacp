#include "Common.h"

#include <eacp/Core/Notifications/NotificationsInternal.h>

using namespace nano;

namespace NotificationsTests
{
namespace Notifications = eacp::Notifications;
using Notifications::ReceivedNotification;

namespace
{
constexpr auto deliveryTimeout = eacp::Time::MS {2000};

ReceivedNotification makeNotification(const std::string& title)
{
    auto notification = ReceivedNotification {};
    notification.title = title;
    notification.body = "finished";
    notification.threadIdentifier = "pane-1";
    notification.userInfo["pane"] = "pane-1";
    return notification;
}
} // namespace

auto tHeldUntilHandlerSet =
    test("Notifications/aTapBeforeAnyHandlerReachesTheHandlerOnceItIsSet") = []
{
    Notifications::setOpenedHandler(nullptr);
    Notifications::Detail::receiveOpenedNotification(makeNotification("launch"));

    auto opened = eacp::Vector<ReceivedNotification> {};
    Notifications::setOpenedHandler([&](const ReceivedNotification& notification)
                                    { opened.add(notification); });

    check(opened.empty());

    auto isDelivered = eacp::Threads::runEventLoopUntil(
        [&] { return !opened.empty(); }, deliveryTimeout);

    check(isDelivered);
    check(opened.size() == 1);
    check(opened[0].title == "launch");
    check(opened[0].threadIdentifier == "pane-1");
    check(opened[0].userInfo.at("pane") == "pane-1");

    Notifications::setOpenedHandler(nullptr);
};

auto tTapReachesHandler =
    test("Notifications/aTapWithAHandlerSetReachesItAtOnce") = []
{
    auto openedTitle = std::string {};
    Notifications::setOpenedHandler([&](const ReceivedNotification& notification)
                                    { openedTitle = notification.title; });

    Notifications::Detail::receiveOpenedNotification(makeNotification("done"));

    check(openedTitle == "done");

    Notifications::setOpenedHandler(nullptr);
};

auto tShownByDefault =
    test("Notifications/aForegroundNotificationIsShownWhenNoHandlerIsSet") = []
{
    Notifications::setForegroundPresentationHandler(nullptr);

    check(Notifications::Detail::shouldPresentInForeground(makeNotification("x")));
};

auto tHandlerDecides =
    test("Notifications/theForegroundHandlerDecidesWhetherItIsShown") = []
{
    Notifications::setForegroundPresentationHandler(
        [](const ReceivedNotification& notification)
        { return notification.title == "shown"; });

    check(
        Notifications::Detail::shouldPresentInForeground(makeNotification("shown")));
    check(!Notifications::Detail::shouldPresentInForeground(
        makeNotification("quiet")));

    Notifications::setForegroundPresentationHandler(nullptr);
};

auto tTokenReachesHandler =
    test("Notifications/aDeviceTokenReachesTheRegisteredHandler") = []
{
    constexpr auto deviceToken = "0aff";

    auto received = std::string {};
    auto handlers = Notifications::RemoteRegistrationHandlers {};
    handlers.onRegistered = [&](const std::string& token) { received = token; };

    Notifications::registerForRemoteNotifications(handlers);
    Notifications::Detail::receiveDeviceToken(deviceToken);

    check(received == deviceToken);

    Notifications::registerForRemoteNotifications({});
};
} // namespace NotificationsTests
