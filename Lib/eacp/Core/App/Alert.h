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

    // The button Return presses. Out of range is the first.
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
// message box where TaskDialogIndirect cannot be found), UIAlertController on
// iOS. Linux and Android have no dialog: there an alert is never shown, only
// logged and answered as unanswered (below).
//
// Returns at once, from any thread. The alert comes up on a later turn of the
// event loop, and `onDismissed` is called on the main thread, exactly once.
// Alerts are shown one at a time, in the order showAlert was called: the next
// comes up only once the one before it has been answered, so an alert raised
// from another's callback follows it rather than stacking over it.
//
// The dialog blocks the main thread while it is up: macOS runs NSAlert's
// runModal and Windows the dialog's own modal loop, as JUCE's
// showMessageBoxAsync does, and that holds inside a plugin host too. This is
// the exception to App.h's note that a modal loop on a host's UI thread is the
// host's to run: eacp's own callbacks and timers still run inside it, the
// host's may not. Windows disables the thread's other top-level windows for as
// long as it is up. iOS presents it over the topmost view controller of the
// foreground scene's key window and blocks nothing.
//
// Unanswered: when nobody chose a button the answer is
// Detail::unansweredButton(), the cancel button if there is one and otherwise
// the default button. That is the answer when the app quits while the alert is
// up, when the dialog fails or is dismissed without a button, when nothing can
// be shown (headless, Linux, Android, iOS with no view controller to present
// from), and when no event loop is running. That last case is a standalone
// app's alert raised on the main thread before Apps::run or after its loop has
// ended: it is logged and answered at once, since no later turn of the loop is
// coming. Other threads cannot tell whether the main thread is running a loop,
// so their alerts are always deferred to it, and a copy inside a plugin always
// defers to the host's loop.
void showAlert(const AlertOptions& options, AlertCallback onDismissed = {});

// Shows each alert in place of the platform's dialog, and in place of the
// headless answer too: how a test sees what an app would have shown and answers
// it. It is handed the options as the platform would be (see
// Detail::resolveAlertOptions) on the main thread and may answer `onDismissed`
// from any thread, at any time; the answer reaches the caller's callback on the
// main thread, and calls after the first are ignored.
//
// The presenter is chosen when an alert's turn comes, not when showAlert was
// called, and the alert holds the queue until the presenter answers it. Setting
// another presenter (nullptr included, which restores the platform's own)
// answers the alert the old one still holds as unanswered, so one that never
// answers holds the queue only for as long as it is installed. Safe to call
// from any thread.
using AlertPresenter =
    std::function<void(const AlertOptions& options, AlertCallback onDismissed)>;

void setAlertPresenter(AlertPresenter presenter);

namespace Detail
{
// The options with the defaults filled in: one "OK" for no buttons, a default
// button in range, a cancel button in range or none.
AlertOptions resolveAlertOptions(const AlertOptions& options);

// The answer when nobody chose: the cancel button, else the default button.
int unansweredButton(const AlertOptions& options);

// The answer for the button at `index` in AlertOptions::buttons, or the
// unanswered one when `index` is no button: a dialog that ended some other way.
int answerForButton(const AlertOptions& options, long index);

// Alerts shown and not yet answered, including those still waiting their turn.
int pendingAlertCount();

// The platform's dialog for resolved options, on the main thread. Calls
// `onDismissed` once with the button pressed.
void showNativeAlert(const AlertOptions& options, AlertCallback onDismissed);

// What an alert that cannot be shown does: logs it and answers the unanswered
// button, at once.
void answerUnshownAlert(const AlertOptions& options,
                        const AlertCallback& onDismissed);

// Windows' answers, kept here rather than in Alert-Windows.cpp so every
// platform's tests pin them down. The ids are Win32's own values, which
// Alert-Windows.cpp checks against the SDK's.
namespace WindowsIds
{
constexpr int ok = 1;
constexpr int cancel = 2;
constexpr int yes = 6;
constexpr int no = 7;

// The task dialog's buttons are this plus their index.
constexpr int firstTaskDialogButton = 100;
} // namespace WindowsIds

// The task dialog's result: its own button ids, and IDCANCEL (Escape or the
// close box, only offered with a cancel button) as the cancel button. Anything
// else is unanswered.
int answerForTaskDialog(const AlertOptions& options, int id);

// MessageBoxW, the fallback, has fixed buttons; this is how an alert's own map
// onto them.
enum class MessageBoxButtons
{
    ok,
    okCancel,
    yesNo,
    yesNoCancel
};

struct MessageBoxPlan
{
    MessageBoxButtons buttons = MessageBoxButtons::ok;

    // The alert's button each box button stands for, in the box's order (OK or
    // Yes, then No, then Cancel). Empty when the alert's buttons cannot be
    // represented, more than three of them: then any answer is unanswered.
    Vector<int> answers;

    // The box button Return presses, as an index into `answers`.
    int defaultBoxButton = 0;

    // Whether the box's labels differ from the alert's, so the message has to
    // say which is which.
    bool labelsInText = false;
};

// One button is OK; two with a cancel button OK and Cancel, Cancel standing for
// the cancel button; two without one Yes and No, which cannot be escaped; three
// Yes, No and Cancel, Cancel standing for the cancel button or else the third.
MessageBoxPlan planMessageBox(const AlertOptions& options);

// The message with the real labels added when the plan needs them.
std::string messageBoxText(const AlertOptions& options, const MessageBoxPlan& plan);

// IDOK/IDYES, IDNO and IDCANCEL (the Cancel button, Escape or the close box)
// through the plan; anything else is unanswered.
int answerForMessageBox(const AlertOptions& options,
                        const MessageBoxPlan& plan,
                        int id);
} // namespace Detail
} // namespace eacp::Apps
