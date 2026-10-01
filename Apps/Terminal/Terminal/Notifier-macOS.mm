#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include "Notifier.h"

#include <eacp/Core/Threads/EventLoop.h>

namespace
{
std::function<void(const std::string&)> activateHandler;
}

@interface TermNotifierDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation TermNotifierDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    NSString* key =
        response.notification.request.content.userInfo[@"session"];
    auto sessionKey = std::string {key != nil ? key.UTF8String : ""};

    eacp::Threads::callAsync(
        [sessionKey]
        {
            if (activateHandler)
                activateHandler(sessionKey);
        });

    completionHandler();
}

// Sessions decide whether a notification is worth posting; once posted it
// should show even while the app is frontmost (the target session may be in
// the background of the same window).
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:
             (void (^)(UNNotificationPresentationOptions))completionHandler
{
    completionHandler(UNNotificationPresentationOptionBanner
                      | UNNotificationPresentationOptionSound);
}

@end

namespace term::Notifier
{
namespace
{
TermNotifierDelegate* delegate = nil;

bool available()
{
    return NSBundle.mainBundle.bundleIdentifier != nil;
}
} // namespace

void initialize(std::function<void(const std::string&)> onActivate)
{
    activateHandler = std::move(onActivate);

    if (!available())
        return;

    delegate = [[TermNotifierDelegate alloc] init];

    auto* center = UNUserNotificationCenter.currentNotificationCenter;
    center.delegate = delegate;

    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert
                                             | UNAuthorizationOptionSound)
                          completionHandler:^(BOOL, NSError*) {
                          }];
}

void attachTray(eacp::Graphics::TrayIcon&) {}

void notify(const std::string& sessionKey,
            const std::string& title,
            const std::string& body)
{
    if (!available())
        return;

    auto* content = [[[UNMutableNotificationContent alloc] init] autorelease];
    content.title = [NSString stringWithUTF8String:title.c_str()];
    content.body = [NSString stringWithUTF8String:body.c_str()];
    content.sound = UNNotificationSound.defaultSound;
    content.userInfo = @{
        @"session": [NSString stringWithUTF8String:sessionKey.c_str()]
    };

    auto* request = [UNNotificationRequest
        requestWithIdentifier:NSUUID.UUID.UUIDString
                      content:content
                      trigger:nil];

    [UNUserNotificationCenter.currentNotificationCenter
        addNotificationRequest:request
         withCompletionHandler:^(NSError*) {
         }];
}
} // namespace term::Notifier
