#include "AppShell.h"
#include "Notifier.h"

#include <eacp/Core/App/App.h>

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
    };

    manager.onSessionsChanged = [this] { wireViews(); };

    manager.onAllClosed = [] { Apps::quit(); };

    manager.onNotify = [this](TermSession& session, const std::string& text)
    { handleSessionNotify(session, text); };

    palette.onClosed = [this] { hidePalette(); };

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

void AppShell::wireViews()
{
    for (auto& session: manager.all())
    {
        auto* raw = session.get();

        session->view.interceptKey = [this](const KeyEvent& event)
        { return interceptKey(event); };

        session->view.onTitleChanged = [this, raw](const std::string&)
        {
            if (manager.active() == raw)
                updateTitle();
        };
    }
}

void AppShell::attachActive(TermSession& session)
{
    if (attached != nullptr)
        removeSubview(attached->view);

    attached = &session;
    addSubview(session.view);
    session.view.setBounds(getLocalBounds());

    if (!palette.isShown())
        session.view.focus();
}

void AppShell::resized()
{
    const auto bounds = getLocalBounds();

    if (attached != nullptr)
        attached->view.setBounds(bounds);

    palette.setBounds(bounds);
}

void AppShell::updateTitle()
{
    auto* session = manager.active();

    if (session == nullptr)
        return;

    auto title = session->name;

    if (!session->view.currentTitle().empty())
        title += " — " + session->view.currentTitle();

    onWindowTitleChanged(title);
}

void AppShell::handleSessionNotify(TermSession& session,
                                   const std::string& text)
{
    // The active, focused session is right in front of the user — no need
    // to shout about it.
    if (manager.active() == &session && session.view.hasFocus())
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
        attached->view.focus();
}

bool AppShell::interceptKey(const KeyEvent& event)
{
    if (prefixArmed)
    {
        prefixArmed = false;
        return handlePrefixed(event);
    }

    if (event.modifiers.control
        && event.charactersIgnoringModifiers == "a"
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

    // Prefix twice sends a literal Ctrl+A through to the shell.
    if (event.modifiers.control && chars == "a")
        return false;

    if (chars == "f" || chars == "w" || chars == "p")
    {
        showPalette();
        return true;
    }

    if (chars == "c")
    {
        if (auto* active = manager.active())
            manager.newSession(active->view.currentCwd());

        return true;
    }

    if (chars == "x")
    {
        if (auto* active = manager.active())
            manager.close(*active);

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
            manager.close(*active);

        return true;
    }

    if (chars == "n")
    {
        if (auto* active = manager.active())
            manager.newSession(active->view.currentCwd());

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
