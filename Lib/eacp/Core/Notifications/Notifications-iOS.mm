#import <UIKit/UIKit.h>

#include "Notifications-Apple.h"
#include "NotificationsInternal.h"

#include "../Threads/EventLoop.h"

namespace eacp::Notifications
{
namespace
{
constexpr auto requestedAuthorizationOptions =
    UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge;

constexpr auto shownInForegroundOptions =
    UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList
    | UNNotificationPresentationOptionSound | UNNotificationPresentationOptionBadge;

UNNotificationPresentationOptions
    getForegroundPresentationOptions(const ReceivedNotification& notification)
{
    if (Detail::shouldPresentInForeground(notification))
        return shownInForegroundOptions;

    return UNNotificationPresentationOptionNone;
}

AuthorizationStatus toAuthorizationStatus(UNAuthorizationStatus status)
{
    switch (status)
    {
        case UNAuthorizationStatusNotDetermined:
            return AuthorizationStatus::notDetermined;
        case UNAuthorizationStatusDenied:
            return AuthorizationStatus::denied;
        case UNAuthorizationStatusAuthorized:
            return AuthorizationStatus::authorized;
        case UNAuthorizationStatusProvisional:
            return AuthorizationStatus::provisional;
        case UNAuthorizationStatusEphemeral:
            return AuthorizationStatus::ephemeral;
    }

    return AuthorizationStatus::notDetermined;
}
} // namespace
} // namespace eacp::Notifications

@interface EACPNotificationCenterDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation EACPNotificationCenterDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    auto notification = eacp::Notifications::Detail::toReceivedNotification(
        response.notification.request.content);

    eacp::Threads::callAsync(
        [notification]
        { eacp::Notifications::Detail::receiveOpenedNotification(notification); });

    completionHandler();
}

// A block, not callAsync: under manual reference counting only a block keeps
// the completion handler alive until the main thread calls it.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)delivered
         withCompletionHandler:
             (void (^)(UNNotificationPresentationOptions))completionHandler
{
    auto notification =
        eacp::Notifications::Detail::toReceivedNotification(delivered.request.content);

    dispatch_async(dispatch_get_main_queue(), ^{
      completionHandler(
          eacp::Notifications::getForegroundPresentationOptions(notification));
    });
}

@end

namespace eacp::Notifications
{
void requestAuthorization(AuthorizationCallback onAnswered)
{
    auto* center = UNUserNotificationCenter.currentNotificationCenter;

    [center requestAuthorizationWithOptions:requestedAuthorizationOptions
                          completionHandler:^(BOOL granted, NSError*) {
                            auto isGranted = granted == YES;

                            Threads::callAsync(
                                [onAnswered, isGranted]
                                {
                                    if (onAnswered)
                                        onAnswered(isGranted);
                                });
                          }];
}

void queryAuthorizationStatus(AuthorizationStatusCallback onAnswered)
{
    auto* center = UNUserNotificationCenter.currentNotificationCenter;

    [center getNotificationSettingsWithCompletionHandler:^(
                UNNotificationSettings* settings) {
      auto status = toAuthorizationStatus(settings.authorizationStatus);

      Threads::callAsync(
          [onAnswered, status]
          {
              if (onAnswered)
                  onAnswered(status);
          });
    }];
}

namespace Detail
{
void startRemoteRegistration()
{
    [UIApplication.sharedApplication registerForRemoteNotifications];
}

void installNotificationCenterDelegate()
{
    static auto* delegate = [[EACPNotificationCenterDelegate alloc] init];
    UNUserNotificationCenter.currentNotificationCenter.delegate = delegate;
}
} // namespace Detail
} // namespace eacp::Notifications
