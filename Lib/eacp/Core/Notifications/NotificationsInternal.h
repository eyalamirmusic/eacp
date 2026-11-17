#pragma once

#include "Notifications.h"

// What the platform's app delegate and notification center delegate call.
namespace eacp::Notifications::Detail
{
void receiveDeviceToken(const std::string& deviceToken);
void receiveRegistrationFailure(const std::string& reason);
void receiveOpenedNotification(const ReceivedNotification& notification);
bool shouldPresentInForeground(const ReceivedNotification& notification);

// The platform's half of registerForRemoteNotifications, once the handlers
// are stored.
void startRemoteRegistration();

// iOS: makes eacp the notification center's delegate. Called from the app
// delegate's didFinishLaunching, the only place that catches the tap which
// launched the app.
void installNotificationCenterDelegate();
} // namespace eacp::Notifications::Detail
