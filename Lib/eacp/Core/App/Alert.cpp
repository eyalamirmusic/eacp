#include "Alert.h"
#include "AppEnvironment.h"
#include "../Platform/Platform.h"
#include "../Threads/EventLoop.h"
#include "../Threads/ThreadUtils.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace eacp::Apps
{
namespace
{
struct PresenterSlot
{
    std::mutex mutex;
    AlertPresenter presenter;
    int generation = 0;
};

PresenterSlot& presenterSlot()
{
    static auto slot = PresenterSlot {};
    return slot;
}

struct WaitingAlert
{
    AlertOptions options;
    AlertCallback onDismissed;
};

std::atomic<int>& pendingAlerts()
{
    static auto count = std::atomic<int> {0};
    return count;
}

bool isButton(const AlertOptions& options, long index)
{
    return index >= 0 && index < (long) options.buttons.size();
}

void runOnMainThread(const Callback& func)
{
    if (Threads::isMainThread())
        func();
    else
        Threads::callAsync(func);
}

void finishAlert(const AlertCallback& onDismissed, int button);

// Behind every AlertCallback a dialog or a presenter is handed: the first
// answer only, from any thread, reaching the caller on the main thread. When
// the last copy of the answer goes without anyone having answered (a UIKit
// alert dismissed by the app, a presenter that dropped it), it answers
// unanswered, posted to the loop rather than run inside whatever destructor
// let it go.
struct AnswerState
{
    AnswerState(AlertCallback onDismissedToUse, int unansweredToUse)
        : onDismissed(std::move(onDismissedToUse))
        , unanswered(unansweredToUse)
    {
    }

    ~AnswerState()
    {
        if (answered.exchange(true))
            return;

        Threads::callAsync([callback = std::move(onDismissed), button = unanswered]
                           { finishAlert(callback, button); });
    }

    AnswerState(const AnswerState&) = delete;
    AnswerState& operator=(const AnswerState&) = delete;

    void answer(int button)
    {
        if (answered.exchange(true))
            return;

        runOnMainThread([callback = onDismissed, button]
                        { finishAlert(callback, button); });
    }

    AlertCallback onDismissed;
    int unanswered;
    std::atomic<bool> answered {false};
};

// Main thread only. `held` is the alert a presenter holds, weakly, so the
// presenter's copies alone keep it alive; `heldBy` is the presenter generation
// it was handed to.
struct AlertQueue
{
    std::deque<WaitingAlert> waiting;
    bool showing = false;
    std::weak_ptr<AnswerState> held;
    int heldBy = -1;
};

AlertQueue& alertQueue()
{
    static auto queue = AlertQueue {};
    return queue;
}

void showNextAlert();

// The next alert is let in before the caller hears the answer, so a callback
// that throws, or pumps the loop waiting on an alert queued behind this one,
// cannot hold the queue. It still comes up on a later turn.
void finishAlert(const AlertCallback& onDismissed, int button)
{
    auto& queue = alertQueue();
    queue.showing = false;
    queue.held.reset();
    queue.heldBy = -1;

    --pendingAlerts();

    Threads::callAsync(showNextAlert);

    if (onDismissed)
        onDismissed(button);
}

std::pair<AlertPresenter, int> currentPresenter()
{
    auto& slot = presenterSlot();
    auto lock = std::lock_guard(slot.mutex);
    return {slot.presenter, slot.generation};
}

void showNextAlert()
{
    auto& queue = alertQueue();

    if (queue.showing || queue.waiting.empty())
        return;

    auto next = std::move(queue.waiting.front());
    queue.waiting.pop_front();
    queue.showing = true;

    auto state = std::make_shared<AnswerState>(
        std::move(next.onDismissed), Detail::unansweredButton(next.options));
    auto weakState = std::weak_ptr<AnswerState>(state);
    auto answer = AlertCallback {[answerState = std::move(state)](int button)
                                 { answerState->answer(button); }};

    auto [presenter, generation] = currentPresenter();

    if (presenter)
    {
        queue.held = weakState;
        queue.heldBy = generation;
        presenter(next.options, std::move(answer));
    }
    else if (getAppEnvironment().headless)
        Detail::answerUnshownAlert(next.options, answer);
    else
        Detail::showNativeAlert(next.options, std::move(answer));
}

// A copy inside a plugin cannot see its host's loop, so it trusts the host to
// run one. isEventLoopRunning() answers for the whole process, so a worker
// sees the main thread's loop too.
bool canDeferAlerts()
{
    return Platform::isDLL() || Threads::isEventLoopRunning();
}
} // namespace

AlertOptions Detail::resolveAlertOptions(const AlertOptions& options)
{
    auto resolved = options;

    if (resolved.buttons.empty())
        resolved.buttons = {"OK"};

    if (!isButton(resolved, resolved.defaultButton))
        resolved.defaultButton = 0;

    if (resolved.cancelButton && !isButton(resolved, *resolved.cancelButton))
        resolved.cancelButton.reset();

    return resolved;
}

int Detail::unansweredButton(const AlertOptions& options)
{
    return options.cancelButton.value_or(options.defaultButton);
}

int Detail::answerForButton(const AlertOptions& options, long index)
{
    return isButton(options, index) ? (int) index : unansweredButton(options);
}

int Detail::pendingAlertCount()
{
    return pendingAlerts().load();
}

int Detail::answerForTaskDialog(const AlertOptions& options, int id)
{
    if (id == WindowsIds::cancel)
        return unansweredButton(options);

    return answerForButton(options, (long) id - WindowsIds::firstTaskDialogButton);
}

namespace
{
const char* boxLabel(Detail::MessageBoxButtons buttons, int position)
{
    using Buttons = Detail::MessageBoxButtons;

    switch (buttons)
    {
        case Buttons::ok:
            return "OK";
        case Buttons::okCancel:
            return position == 0 ? "OK" : "Cancel";
        case Buttons::yesNo:
            return position == 0 ? "Yes" : "No";
        case Buttons::abortRetryIgnore:
            return position == 0 ? "Abort" : position == 1 ? "Retry" : "Ignore";
        case Buttons::yesNoCancel:
            break;
    }

    return position == 0 ? "Yes" : position == 1 ? "No" : "Cancel";
}

Vector<int> buttonsOtherThan(const AlertOptions& options, int excluded)
{
    auto others = Vector<int> {};

    for (auto index = 0; index < options.buttons.size(); ++index)
        if (index != excluded)
            others.add(index);

    return others;
}
} // namespace

Detail::MessageBoxPlan Detail::planMessageBox(const AlertOptions& options)
{
    auto plan = MessageBoxPlan {};
    auto count = options.buttons.size();

    if (count > 3)
    {
        plan.labelsInText = true;
        return plan;
    }

    if (count == 1)
    {
        plan.buttons = MessageBoxButtons::ok;
        plan.answers = {0};
    }
    else if (options.cancelButton)
    {
        plan.buttons = count == 2 ? MessageBoxButtons::okCancel
                                  : MessageBoxButtons::yesNoCancel;
        plan.answers = buttonsOtherThan(options, *options.cancelButton);
        plan.answers.add(*options.cancelButton);
    }
    else
    {
        plan.buttons = count == 2 ? MessageBoxButtons::yesNo
                                  : MessageBoxButtons::abortRetryIgnore;
        plan.answers = buttonsOtherThan(options, -1);
    }

    for (auto position = 0; position < plan.answers.size(); ++position)
    {
        if (plan.answers[position] == options.defaultButton)
            plan.defaultBoxButton = position;

        if (options.buttons[plan.answers[position]]
            != boxLabel(plan.buttons, position))
            plan.labelsInText = true;
    }

    return plan;
}

std::string Detail::messageBoxText(const AlertOptions& options,
                                   const MessageBoxPlan& plan)
{
    if (!plan.labelsInText)
        return options.message;

    auto text = options.message + "\n\n";

    if (plan.answers.empty())
    {
        text += "This message box cannot offer the buttons:";

        for (auto& label: options.buttons)
            text += " " + label + ";";

        return text;
    }

    for (auto position = 0; position < plan.answers.size(); ++position)
    {
        if (position > 0)
            text += ", ";

        text += std::string(boxLabel(plan.buttons, position)) + " = "
                + options.buttons[plan.answers[position]];
    }

    return text;
}

int Detail::answerForMessageBox(const AlertOptions& options,
                                const MessageBoxPlan& plan,
                                int id)
{
    auto position = -1;

    switch (plan.buttons)
    {
        case MessageBoxButtons::ok:
            position = id == WindowsIds::ok ? 0 : -1;
            break;
        case MessageBoxButtons::okCancel:
            position = id == WindowsIds::ok ? 0 : id == WindowsIds::cancel ? 1 : -1;
            break;
        case MessageBoxButtons::yesNo:
            position = id == WindowsIds::yes ? 0 : id == WindowsIds::no ? 1 : -1;
            break;
        case MessageBoxButtons::yesNoCancel:
            position = id == WindowsIds::yes      ? 0
                       : id == WindowsIds::no     ? 1
                       : id == WindowsIds::cancel ? 2
                                                  : -1;
            break;
        case MessageBoxButtons::abortRetryIgnore:
            position = id == WindowsIds::abort    ? 0
                       : id == WindowsIds::retry  ? 1
                       : id == WindowsIds::ignore ? 2
                                                  : -1;
            break;
    }

    if (position < 0 || position >= plan.answers.size())
        return unansweredButton(options);

    return plan.answers[position];
}

void Detail::answerUnshownAlert(const AlertOptions& options,
                                const AlertCallback& onDismissed)
{
    LOG("Alert (not shown): ", options.title, ": ", options.message);
    onDismissed(unansweredButton(options));
}

void showAlert(const AlertOptions& options, AlertCallback onDismissed)
{
    auto resolved = Detail::resolveAlertOptions(options);

    if (!canDeferAlerts())
    {
        LOG("Alert (no event loop running, not shown): ",
            resolved.title,
            ": ",
            resolved.message);

        if (onDismissed)
            onDismissed(Detail::unansweredButton(resolved));

        return;
    }

    ++pendingAlerts();

    Threads::callAsync(
        [alert = WaitingAlert {std::move(resolved), std::move(onDismissed)}]
        {
            alertQueue().waiting.push_back(alert);
            showNextAlert();
        });
}

// The old presenter is destroyed after the lock is released, since what it
// captures may call back in here from its destructor.
void setAlertPresenter(AlertPresenter presenter)
{
    auto replaced = 0;
    auto old = AlertPresenter {};

    {
        auto& slot = presenterSlot();
        auto lock = std::lock_guard(slot.mutex);
        old = std::exchange(slot.presenter, std::move(presenter));
        replaced = slot.generation++;
    }

    runOnMainThread(
        [replaced]
        {
            auto& queue = alertQueue();

            if (queue.heldBy > replaced)
                return;

            if (auto held = queue.held.lock())
                held->answer(held->unanswered);
        });
}
} // namespace eacp::Apps
