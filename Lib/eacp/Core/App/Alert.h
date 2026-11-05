#pragma once

#include "../Utils/Common.h"

#include <optional>

namespace eacp::Apps
{
// The symbol beside the message. macOS shows the app's icon either way and
// badges it with a caution sign for `error`; iOS has no icons in alerts.
enum class AlertIcon
{
    info,
    warning,
    error
};

struct AlertOptions
{
    std::string title;
    std::string message;
    AlertIcon icon = AlertIcon::info;

    // The answers, the first being the primary one. Each platform lays them out
    // its own way: macOS right to left, Windows and iOS left to right. Empty is
    // one "OK".
    Vector<std::string> buttons;

    // The button Return presses, and the answer wherever no dialog can be
    // shown. Out of range is the first.
    int defaultButton = 0;

    // The button Escape presses, which is also what closing the dialog answers
    // where the platform lets it be closed. Unset, or out of range, the alert
    // has no cancel button and Escape does nothing.
    std::optional<int> cancelButton;
};

// The index of the button pressed, into AlertOptions::buttons (0 for the "OK"
// an empty list becomes).
using AlertCallback = std::function<void(int button)>;

// Shows a native message box: NSAlert on macOS, a task dialog on Windows (a
// message box where comctl32 v6 cannot be loaded), UIAlertController on iOS.
// Returns at once, from any thread; the alert comes up on the next turn of the
// event loop, which must be running, and `onDismissed` is called there, on the
// main thread, exactly once.
//
// macOS and Windows run the alert app-modally from the event loop, as JUCE's
// showMessageBoxAsync does. iOS presents it over the topmost view controller of
// the foreground scene's key window.
//
// Where nothing can be shown the alert is logged and answered with the default
// button, still on the next turn of the loop: headless (AppEnvironment), on
// Linux and Android, and on iOS with no view controller to present from.
void showAlert(const AlertOptions& options, AlertCallback onDismissed = {});

// Shows each alert in place of the platform's dialog, and in place of the
// headless answer too: how a test sees what an app would have shown and answers
// it. It is handed the options as the platform would be (see
// Detail::resolveAlertOptions) on the main thread, and must call `onDismissed`
// once, at any time; calls after the first are ignored. Pass nullptr to restore
// the platform's own.
using AlertPresenter =
    std::function<void(const AlertOptions& options, AlertCallback onDismissed)>;

void setAlertPresenter(AlertPresenter presenter);

namespace Detail
{
// The options with the defaults filled in: one "OK" for no buttons, a default
// button in range, a cancel button in range or none.
AlertOptions resolveAlertOptions(const AlertOptions& options);

// The platform's dialog for resolved options, on the main thread. Calls
// `onDismissed` once with the button pressed.
void showNativeAlert(const AlertOptions& options, AlertCallback onDismissed);

// What an alert that cannot be shown does: logs it and answers the default
// button, at once.
void answerUnshownAlert(const AlertOptions& options,
                        const AlertCallback& onDismissed);
} // namespace Detail
} // namespace eacp::Apps
