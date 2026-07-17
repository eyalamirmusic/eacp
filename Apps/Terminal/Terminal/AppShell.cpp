#include "AppShell.h"
#include "Notifier.h"

#include <eacp/Core/App/App.h>

#include <cctype>

namespace term
{
using namespace eacp;
using Graphics::KeyEvent;
namespace KeyCode = Graphics::KeyCode;

AppShell::AppShell()
{
    manager.onActiveChanged = [this](TermSession& session)
    {
        attachActive(session);
        updateTitle();
        tray.refresh();
    };

    manager.onSessionsChanged = [this] { tray.refresh(); };

    manager.onPaneWired = [this](TermSession& session, TerminalView& pane)
    {
        auto* raw = &session;

        pane.interceptKey = [this](const KeyEvent& event)
        { return interceptKey(event); };

        pane.onTitleChanged = [this, raw](const std::string&)
        {
            if (manager.active() == raw)
                updateTitle();
        };
    };

    tray.onShowWindow = [this] { onBringToFront(); };

    tray.onPickSession = [this](TermSession& session)
    {
        manager.switchTo(session);
        onBringToFront();
    };

    manager.onAllClosed = [] { Apps::quit(); };

    manager.onNotify = [this](TermSession& session, const std::string& text)
    {
        handleSessionNotify(session, text);
        tray.refresh();
    };

    palette.onClosed = [this] { hidePalette(); };

    popup.onClosed = [this] { hidePopup(); };

    popup.interceptKey = [this](const KeyEvent& event) { return popupKey(event); };

    Notifier::initialize(
        [this](const std::string& sessionKey)
        {
            if (auto* session = manager.find(sessionKey))
            {
                manager.switchTo(*session);
                onBringToFront();
            }
        });
}

void AppShell::start()
{
    manager.restoreOrCreateInitial();
}

void AppShell::attachActive(TermSession& session)
{
    if (attached != &session)
    {
        if (attached != nullptr)
            removeSubview(attached->view);

        attached = &session;
        addSubview(session.view);
    }

    session.view.setBounds(getLocalBounds());

    if (!palette.isShown())
        session.view.focusActive();
}

void AppShell::resized()
{
    const auto bounds = getLocalBounds();

    if (attached != nullptr)
        attached->view.setBounds(bounds);

    palette.setBounds(bounds);
    popup.setBounds(bounds);
}

void AppShell::updateTitle()
{
    auto* session = manager.active();

    if (session == nullptr)
        return;

    auto title = session->name;

    if (!session->activeTitle().empty())
        title += " — " + session->activeTitle();

    onWindowTitleChanged(title);
}

void AppShell::handleSessionNotify(TermSession& session, const std::string& text)
{
    // The active, focused session is right in front of the user — no need
    // to shout about it.
    const auto* pane = const_cast<const SessionView&>(session.view).activePane();

    if (windowFocused && manager.active() == &session && pane != nullptr
        && pane->hasFocus())
        return;

    Notifier::notify(session.key(), session.name, text);
}

void AppShell::showPalette()
{
    if (palette.isShown())
        return;

    addSubview(palette);
    palette.setBounds(getLocalBounds());
    palette.show();
    palette.focus();
}

void AppShell::hidePalette()
{
    removeSubview(palette);

    if (attached != nullptr)
        attached->view.focusActive();
}

void AppShell::showPopup(const std::string& command)
{
    if (popup.isShown())
        return;

    auto* active = manager.active();

    const auto dir =
        active != nullptr ? active->activeWorkingDirectory() : std::string {};

    popupPrefixArmed = false;
    addSubview(popup);
    popup.setBounds(getLocalBounds());
    popup.show(command, dir);
}

void AppShell::hidePopup()
{
    removeSubview(popup);

    if (attached != nullptr)
        attached->view.focusActive();
}

bool AppShell::popupKey(const KeyEvent& event)
{
    if (popupPrefixArmed)
    {
        popupPrefixArmed = false;

        if (event.modifiers.control && event.charactersIgnoringModifiers == "a")
            return false;

        const auto togglesPopup = [&](const std::string& chars)
        {
            if (chars == "i")
                return true;

            for (const auto& binding: config.bindings)
                if (!binding.popup.empty() && chars == binding.key)
                    return true;

            return false;
        };

        if (togglesPopup(event.charactersIgnoringModifiers))
        {
            popup.dismiss();
            return true;
        }

        // The rest of the leader table targets panes hidden behind the
        // popup; swallow rather than mutate what the user can't see.
        return true;
    }

    if (event.modifiers.control && event.charactersIgnoringModifiers == "a"
        && !event.modifiers.command)
    {
        popupPrefixArmed = true;
        return true;
    }

    return false;
}

bool AppShell::interceptKey(const KeyEvent& event)
{
    if (prefixArmed)
    {
        prefixArmed = false;
        return handlePrefixed(event);
    }

    if (event.modifiers.control && event.charactersIgnoringModifiers == "a"
        && !event.modifiers.command)
    {
        prefixArmed = true;
        return true;
    }

    if (event.modifiers.command)
        return handleCommand(event);

    return false;
}

bool AppShell::handlePrefixed(const KeyEvent& event)
{
    const auto& chars = event.charactersIgnoringModifiers;
    auto* active = manager.active();
    auto* paneTree = active != nullptr ? &active->view : nullptr;

    // Prefix twice sends a literal Ctrl+A through to the shell.
    if (event.modifiers.control && chars == "a")
        return false;

    // Config bindings first, so ~/.config/cowterm.json can override built-ins.
    for (const auto& binding: config.bindings)
    {
        if (binding.key.empty() || chars != binding.key)
            continue;

        if (!binding.send.empty())
        {
            if (paneTree != nullptr)
                if (auto* pane = paneTree->activePane())
                    pane->sendText(binding.send);
        }
        else if (!binding.popup.empty())
        {
            showPopup(binding.popup);
        }

        return true;
    }

    // Shift+H/J/K/L: resize the active pane by one cell. Ctrl is left out
    // of resize on purpose: rolling the prefix leaves it held, so Ctrl+h
    // must mean "move focus", never resize.
    if (chars == "H" || chars == "J" || chars == "K" || chars == "L")
    {
        if (paneTree != nullptr)
            paneTree->resizeActive((char) std::tolower((unsigned char) chars[0]),
                                   1.0f);

        return true;
    }

    // Arrows: plain moves focus (tmux select-pane); Ctrl resizes by one
    // cell, Alt by five.
    const auto arrowDirection = [&]() -> char
    {
        switch (event.keyCode)
        {
            case KeyCode::LeftArrow:
                return 'h';
            case KeyCode::DownArrow:
                return 'j';
            case KeyCode::UpArrow:
                return 'k';
            case KeyCode::RightArrow:
                return 'l';
            default:
                return 0;
        }
    }();

    if (arrowDirection != 0)
    {
        if (paneTree != nullptr)
        {
            if (event.modifiers.control || event.modifiers.alt)
                paneTree->resizeActive(arrowDirection,
                                       event.modifiers.alt ? 5.0f : 1.0f);
            else
                paneTree->focusDirection(arrowDirection);
        }

        return true;
    }

    if (chars == "f" || chars == "w" || chars == "p")
    {
        showPalette();
        return true;
    }

    // Lazygit over the active pane's directory, tmux display-popup style;
    // quitting lazygit (or Ctrl+A i again) dismisses it.
    if (chars == "i")
    {
        showPopup("lazygit");
        return true;
    }

    // Pane splits, in the pane's current directory: " below, % beside.
    if (chars == "\"")
    {
        if (paneTree != nullptr)
            paneTree->splitActive(false);

        return true;
    }

    if (chars == "%")
    {
        if (paneTree != nullptr)
            paneTree->splitActive(true);

        return true;
    }

    // Focus moves whether or not Ctrl is still down from the prefix roll.
    if (chars == "h" || chars == "j" || chars == "k" || chars == "l")
    {
        if (paneTree != nullptr)
            paneTree->focusDirection(chars[0]);

        return true;
    }

    if (chars == "z")
    {
        if (paneTree != nullptr)
            paneTree->toggleZoom();

        return true;
    }

    if (chars == "o")
    {
        if (paneTree != nullptr)
            paneTree->cycleFocus();

        return true;
    }

    if (chars == "c")
    {
        if (active != nullptr)
            manager.newSession(active->activeWorkingDirectory());

        return true;
    }

    // Kill the active pane; the session ends with its last pane.
    if (chars == "x")
    {
        if (paneTree != nullptr)
            paneTree->closeActivePane();

        return true;
    }

    if (chars == "^")
    {
        manager.switchToLast();
        return true;
    }

    if (chars.size() == 1 && chars[0] >= '1' && chars[0] <= '9')
    {
        manager.switchToIndex(chars[0] - '1');
        return true;
    }

    return true;
}

bool AppShell::handleCommand(const KeyEvent& event)
{
    const auto& chars = event.charactersIgnoringModifiers;

    if (chars == "k" || chars == "t")
    {
        showPalette();
        return true;
    }

    if (chars == "w")
    {
        if (auto* active = manager.active())
            active->view.closeActivePane();

        return true;
    }

    if (chars == "n")
    {
        if (auto* active = manager.active())
            manager.newSession(active->activeWorkingDirectory());

        return true;
    }

    if (chars == "d")
    {
        if (auto* active = manager.active())
            active->view.splitActive(true);

        return true;
    }

    if (chars == "q")
    {
        Apps::quit();
        return true;
    }

    if (chars.size() == 1 && chars[0] >= '1' && chars[0] <= '9')
    {
        manager.switchToIndex(chars[0] - '1');
        return true;
    }

    return false;
}
} // namespace term
