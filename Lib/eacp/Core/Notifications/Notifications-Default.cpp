#include "NotificationsInternal.h"

namespace eacp::Notifications
{
namespace
{
constexpr auto unavailableReason =
    "notifications are not available on this platform";
}

void requestAuthorization(AuthorizationCallback onAnswered)
{
    if (onAnswered)
        onAnswered(false);
}

void queryAuthorizationStatus(AuthorizationStatusCallback onAnswered)
{
    if (onAnswered)
        onAnswered(AuthorizationStatus::unavailable);
}

namespace Detail
{
void startRemoteRegistration()
{
    receiveRegistrationFailure(unavailableReason);
}
} // namespace Detail
} // namespace eacp::Notifications
