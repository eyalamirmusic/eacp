#pragma once

#include "../Utils/Common.h"

#include <map>

// Notifications the system shows for the app: the user's permission, the
// device token a push server sends to, and the taps on what was delivered.
// Implemented on iOS with UNUserNotificationCenter. Everywhere else it is
// unavailable: permission is never granted and registration always fails, so
// code calling it builds and runs on every platform.
//
// Every callback runs on the main thread. Call these from the main thread too.
namespace eacp::Notifications
{
enum class AuthorizationStatus
{
    notDetermined,
    denied,
    authorized,
    provisional,
    ephemeral,
    unavailable,
};

// A delivered notification: one the user tapped, or one arriving while the
// app is in the foreground.
struct ReceivedNotification
{
    std::string title;
    std::string body;
    std::string threadIdentifier;

    // The payload's own keys, beside "aps". A string value is kept as it is, a
    // number or boolean is written out ("42", "1"), and an object or array is
    // left out.
    std::map<std::string, std::string> userInfo;
};

using AuthorizationCallback = std::function<void(bool isGranted)>;
using AuthorizationStatusCallback = std::function<void(AuthorizationStatus status)>;
using OpenedHandler = std::function<void(const ReceivedNotification& notification)>;
using ForegroundPresentationHandler =
    std::function<bool(const ReceivedNotification& notification)>;

// Asks the user to allow alerts, sounds and badges. The first call shows the
// system's prompt; later ones answer with the user's earlier choice without
// asking again. Answers false where notifications are unavailable, at once.
void requestAuthorization(AuthorizationCallback onAnswered);

// The user's current choice, without asking. Answers unavailable at once where
// notifications are unavailable.
void queryAuthorizationStatus(AuthorizationStatusCallback onAnswered);

struct RemoteRegistrationHandlers
{
    // The device token as lowercase hex, which a push server addresses this
    // install by. Called again whenever the system hands the app a token,
    // which can change between launches.
    std::function<void(const std::string& deviceToken)> onRegistered;

    // Why registration failed, as the system words it. Called at once where
    // notifications are unavailable.
    std::function<void(const std::string& reason)> onFailed;
};

// Registers this install for push notifications. The app also needs the
// aps-environment entitlement in its signing profile, or registration fails.
void registerForRemoteNotifications(RemoteRegistrationHandlers handlers);

// Called when the user taps a delivered notification. A tap that arrives before
// a handler is set, as the one that launched the app does, is held and handed
// to the handler on a later turn of the event loop once it is set.
void setOpenedHandler(OpenedHandler handler);

// Decides whether a notification arriving while the app is in the foreground
// is shown, with its banner and its sound; return false to keep it quiet.
// Unset, every one is shown. The sound is whatever the payload names.
void setForegroundPresentationHandler(ForegroundPresentationHandler handler);
} // namespace eacp::Notifications
