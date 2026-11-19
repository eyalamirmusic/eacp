#include "NotificationsInternal.h"

#include "../Threads/EventLoop.h"
#include "../Utils/Singleton.h"

namespace eacp::Notifications
{
namespace
{
struct NotificationHandlers
{
    OpenedHandler onOpened;
    ForegroundPresentationHandler shouldPresentInForeground;
    RemoteRegistrationHandlers remoteRegistration;
    Vector<ReceivedNotification> heldOpenedNotifications;
};

NotificationHandlers& getHandlers()
{
    return Singleton::get<NotificationHandlers>();
}

void deliverHeldOpenedNotifications()
{
    auto& handlers = getHandlers();
    auto held = std::move(handlers.heldOpenedNotifications);
    handlers.heldOpenedNotifications = {};

    for (const auto& notification: held)
        Detail::receiveOpenedNotification(notification);
}
} // namespace

void registerForRemoteNotifications(RemoteRegistrationHandlers handlers)
{
    getHandlers().remoteRegistration = std::move(handlers);
    Detail::startRemoteRegistration();
}

void setOpenedHandler(OpenedHandler handler)
{
    auto& handlers = getHandlers();
    handlers.onOpened = std::move(handler);

    if (handlers.onOpened && !handlers.heldOpenedNotifications.empty())
        Threads::callAsync(deliverHeldOpenedNotifications);
}

void setForegroundPresentationHandler(ForegroundPresentationHandler handler)
{
    getHandlers().shouldPresentInForeground = std::move(handler);
}

namespace Detail
{
void receiveDeviceToken(const std::string& deviceToken)
{
    const auto& onRegistered = getHandlers().remoteRegistration.onRegistered;

    if (onRegistered)
        onRegistered(deviceToken);
}

void receiveRegistrationFailure(const std::string& reason)
{
    const auto& onFailed = getHandlers().remoteRegistration.onFailed;

    if (onFailed)
        onFailed(reason);
}

void receiveOpenedNotification(const ReceivedNotification& notification)
{
    auto& handlers = getHandlers();

    if (!handlers.onOpened)
    {
        handlers.heldOpenedNotifications.add(notification);
        return;
    }

    handlers.onOpened(notification);
}

bool shouldPresentInForeground(const ReceivedNotification& notification)
{
    const auto& shouldPresent = getHandlers().shouldPresentInForeground;

    if (!shouldPresent)
        return true;

    return shouldPresent(notification);
}
} // namespace Detail
} // namespace eacp::Notifications
