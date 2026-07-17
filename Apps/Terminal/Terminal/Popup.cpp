#include "Popup.h"

#include <eacp/Core/Threads/EventLoop.h>

namespace term
{
using namespace eacp;
using Graphics::Color;
using Graphics::Context;
using Graphics::Rect;

namespace
{
constexpr float margin = 10.0f;
}

Popup::Popup(const AppConfig& configToUse)
    : config(configToUse)
    , theme(themeByName(configToUse.theme))
{
    setHandlesMouseEvents(true);
}

void Popup::show(const std::string& command, const std::string& workingDirectory)
{
    if (terminal != nullptr)
        return;

    terminal = std::make_unique<TerminalView>(config, workingDirectory, "", command);

    terminal->interceptKey = [this](const Graphics::KeyEvent& event)
    { return interceptKey(event); };

    terminal->onShellExit = [this] { dismiss(); };

    addSubview(*terminal);
    resized();
    terminal->focus();
    repaint();
}

void Popup::dismiss()
{
    if (terminal == nullptr || closing)
        return;

    closing = true;
    terminal->terminateShell();

    // The terminal must not die here: dismiss() runs inside its own keyDown
    // or shell-exit callback, and a std::function must not die
    // mid-invocation.
    Threads::callAsync(
        [this, guard = std::weak_ptr<bool> {alive}]
        {
            if (guard.expired())
                return;

            removeSubview(*terminal);
            terminal.reset();
            closing = false;
            onClosed();
        });
}

void Popup::resized()
{
    if (terminal == nullptr)
        return;

    const auto panel = panelBounds();
    terminal->setBounds(
        {panel.x + 1.0f, panel.y + 1.0f, panel.w - 2.0f, panel.h - 2.0f});
}

Rect Popup::panelBounds() const
{
    const auto bounds = getLocalBounds();

    return {bounds.x + margin,
            bounds.y + margin,
            bounds.w - 2.0f * margin,
            bounds.h - 2.0f * margin};
}

void Popup::paint(Context& context)
{
    context.setColor(Color::black(0.38f));
    context.fillRect(getLocalBounds());

    const auto panel = panelBounds();
    context.setColor(toColor(theme.background));
    context.fillRect(panel);

    context.setColor(toColor(theme.paneBorderActive));
    context.setLineWidth(1.0f);
    context.strokeRect(panel);
}
} // namespace term
