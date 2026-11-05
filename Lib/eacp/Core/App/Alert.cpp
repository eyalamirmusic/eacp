#include "Alert.h"
#include "AppEnvironment.h"
#include "../Threads/EventLoop.h"

#include <atomic>
#include <memory>

namespace eacp::Apps
{
namespace
{
AlertPresenter& alertPresenter()
{
    static auto presenter = AlertPresenter {};
    return presenter;
}

bool isButton(const AlertOptions& options, int index)
{
    return index >= 0 && index < options.buttons.size();
}

AlertCallback answeredOnce(AlertCallback onDismissed)
{
    auto answered = std::make_shared<std::atomic<bool>>(false);

    return [answered, onDismissed = std::move(onDismissed)](int button)
    {
        if (answered->exchange(true))
            return;

        if (onDismissed)
            onDismissed(button);
    };
}

void presentAlert(const AlertOptions& options, const AlertCallback& onDismissed)
{
    if (auto presenter = alertPresenter())
        presenter(options, onDismissed);
    else if (getAppEnvironment().headless)
        Detail::answerUnshownAlert(options, onDismissed);
    else
        Detail::showNativeAlert(options, onDismissed);
}
} // namespace

void Detail::answerUnshownAlert(const AlertOptions& options,
                                const AlertCallback& onDismissed)
{
    LOG("Alert (not shown): ", options.title, ": ", options.message);
    onDismissed(options.defaultButton);
}

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

void showAlert(const AlertOptions& options, AlertCallback onDismissed)
{
    Threads::callAsync([options = Detail::resolveAlertOptions(options),
                        onDismissed = answeredOnce(std::move(onDismissed))]
                       { presentAlert(options, onDismissed); });
}

void setAlertPresenter(AlertPresenter presenter)
{
    alertPresenter() = std::move(presenter);
}
} // namespace eacp::Apps
