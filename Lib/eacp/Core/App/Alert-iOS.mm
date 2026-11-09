#import <UIKit/UIKit.h>

#include "Alert.h"
#include "../ObjC/Strings.h"
#include "../Threads/EventLoop.h"

namespace eacp::Apps
{
// How long an alert waits for somewhere to be presented from: a scene still
// connecting at launch, a view controller mid-transition.
constexpr auto presentRetries = 50;
constexpr auto presentRetryDelay = Time::MS {100};

static UIViewController* topmostOf(UIViewController* controller)
{
    while (controller.presentedViewController != nil)
        controller = controller.presentedViewController;

    return controller;
}

static UIWindow* keyWindowOf(UIWindowScene* scene)
{
    for (UIWindow* window in scene.windows)
    {
        if (window.isKeyWindow)
            return window;
    }

    return scene.windows.firstObject;
}

// The key window of the foreground scene, else of any scene, so an alert
// raised while the app is coming back still finds somewhere to go.
static UIViewController* presentingController()
{
    auto* fallback = (UIViewController*) nil;

    for (UIScene* scene in [UIApplication sharedApplication].connectedScenes)
    {
        if (! [scene isKindOfClass:[UIWindowScene class]])
            continue;

        auto* root = keyWindowOf((UIWindowScene*) scene).rootViewController;

        if (root == nil)
            continue;

        if (scene.activationState == UISceneActivationStateForegroundActive)
            return topmostOf(root);

        if (fallback == nil)
            fallback = root;
    }

    return fallback != nil ? topmostOf(fallback) : nil;
}

// UIKit refuses, with only a console warning, to present from a controller
// that is itself being presented or dismissed, as an alert raised from another
// alert's handler finds the first one.
static bool canPresentFrom(UIViewController* controller)
{
    return controller != nil && ! controller.isBeingPresented
           && ! controller.isBeingDismissed;
}

static NSString* alertText(const std::string& text)
{
    auto* string = Strings::toNSString(text);
    return string != nil ? string : @"";
}

static UIAlertActionStyle actionStyleFor(const AlertOptions& options, int index)
{
    if (options.cancelButton == index)
        return UIAlertActionStyleCancel;

    return UIAlertActionStyleDefault;
}

static UIAlertController* makeAlert(const AlertOptions& options,
                                    const AlertCallback& onDismissed)
{
    auto* alert = [UIAlertController
        alertControllerWithTitle:alertText(options.title)
                         message:alertText(options.message)
                  preferredStyle:UIAlertControllerStyleAlert];

    for (auto index = 0; index < options.buttons.size(); ++index)
    {
        auto* action = [UIAlertAction actionWithTitle:alertText(options.buttons[index])
                                                style:actionStyleFor(options, index)
                                              handler:^(UIAlertAction*) {
                                                onDismissed(index);
                                              }];
        [alert addAction:action];

        if (index == options.defaultButton)
            alert.preferredAction = action;
    }

    return alert;
}

// Tries again on a later turn while nothing can present it, and answers it as
// unanswered once the retries run out, so a refused presentation never leaves
// the caller waiting.
static void presentAlert(const AlertOptions& options,
                         const AlertCallback& onDismissed,
                         int retriesLeft)
{
    auto retry = [&]
    {
        if (retriesLeft == 0)
        {
            Detail::answerUnshownAlert(options, onDismissed);
            return;
        }

        Threads::callAfter(presentRetryDelay,
                           [options, onDismissed, retriesLeft]
                           { presentAlert(options, onDismissed, retriesLeft - 1); });
    };

    auto* presenter = presentingController();

    if (! canPresentFrom(presenter))
    {
        retry();
        return;
    }

    auto* alert = makeAlert(options, onDismissed);
    [presenter presentViewController:alert animated:YES completion:nil];

    if (alert.presentingViewController == nil)
        retry();
}

// iOS alerts carry no icon; AlertOptions::icon has nothing to set here.
void Detail::showNativeAlert(const AlertOptions& options, AlertCallback onDismissed)
{
    presentAlert(options, onDismissed, presentRetries);
}
} // namespace eacp::Apps
