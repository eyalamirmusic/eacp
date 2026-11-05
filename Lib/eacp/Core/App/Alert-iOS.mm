#import <UIKit/UIKit.h>

#include "Alert.h"
#include "../ObjC/Strings.h"

namespace eacp::Apps
{
static UIViewController* topmostOf(UIViewController* controller)
{
    while (controller.presentedViewController != nil
           && ! controller.presentedViewController.isBeingDismissed)
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

// iOS alerts carry no icon; AlertOptions::icon has nothing to set here.
void Detail::showNativeAlert(const AlertOptions& options, AlertCallback onDismissed)
{
    auto* presenter = presentingController();

    if (presenter == nil)
    {
        answerUnshownAlert(options, onDismissed);
        return;
    }

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

    [presenter presentViewController:alert animated:YES completion:nil];
}
} // namespace eacp::Apps
