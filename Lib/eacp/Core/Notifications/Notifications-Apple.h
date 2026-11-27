#pragma once

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include "Notifications.h"

namespace eacp::Notifications::Detail
{
std::string toDeviceTokenHex(NSData* deviceToken);
std::map<std::string, std::string> toUserInfoStrings(NSDictionary* userInfo);
ReceivedNotification toReceivedNotification(UNNotificationContent* content);
} // namespace eacp::Notifications::Detail
