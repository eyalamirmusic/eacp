#include "Common.h"

using namespace nano;

namespace NotificationsDefaultTests
{
namespace Notifications = eacp::Notifications;

auto tNotGranted =
    test("Notifications/permissionIsNeverGrantedWhereUnavailable") = []
{
    auto answers = 0;
    auto isGranted = true;

    Notifications::requestAuthorization(
        [&](bool granted)
        {
            ++answers;
            isGranted = granted;
        });

    check(answers == 1);
    check(!isGranted);
};

auto tUnavailable =
    test("Notifications/authorizationStatusIsUnavailableWhereUnavailable") = []
{
    auto status = Notifications::AuthorizationStatus::authorized;

    Notifications::queryAuthorizationStatus(
        [&](Notifications::AuthorizationStatus answered) { status = answered; });

    check(status == Notifications::AuthorizationStatus::unavailable);
};

auto tRegistrationFails =
    test("Notifications/remoteRegistrationFailsWhereUnavailable") = []
{
    auto isRegistered = false;
    auto reason = std::string {};

    auto handlers = Notifications::RemoteRegistrationHandlers {};
    handlers.onRegistered = [&](const std::string&) { isRegistered = true; };
    handlers.onFailed = [&](const std::string& failure) { reason = failure; };

    Notifications::registerForRemoteNotifications(handlers);

    check(!isRegistered);
    check(!reason.empty());

    Notifications::registerForRemoteNotifications({});
};
} // namespace NotificationsDefaultTests
