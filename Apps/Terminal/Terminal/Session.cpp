#include "Session.h"
#include "Projects.h"

#include <emberstore/AppDatabase.h>

#include <algorithm>

namespace term
{
namespace
{
std::string lowered(std::string text)
{
    std::transform(text.begin(),
                   text.end(),
                   text.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    return text;
}

std::string normalizedDir(const std::string& dir)
{
    auto path = expandHome(dir);

    while (path.size() > 1 && path.back() == '/')
        path.pop_back();

    return path;
}
} // namespace

TermSession::TermSession(const AppConfig& config,
                         std::string nameToUse,
                         std::string projectDirToUse,
                         const std::string& startCwd)
    : name(std::move(nameToUse))
    , projectDir(std::move(projectDirToUse))
    , view(config, startCwd.empty() ? projectDir : startCwd)
{
}

bool TermSession::isClaude() const
{
    if (lowered(view.foregroundProcess()).find("claude") != std::string::npos)
        return true;

    return lowered(view.currentTitle()).find("claude") != std::string::npos;
}

SessionManager::SessionManager(const AppConfig& configToUse)
    : config(configToUse)
    , db(emberstore::databaseForApp(
          "tamber", "wim-terminal", emberstore::Durability::Atomic))
    , mru(db)
    , saved(db.document<SavedState>("sessions"))
{
}

TermSession* SessionManager::find(const std::string& key)
{
    for (auto& session: sessions)
        if (session->key() == key)
            return session.get();

    return nullptr;
}

std::string SessionManager::uniqueName(const std::string& base) const
{
    auto name = base;
    auto suffix = 2;

    auto taken = [&]
    {
        for (auto& session: sessions)
            if (session->name == name)
                return true;

        return false;
    };

    while (taken())
        name = base + " " + std::to_string(suffix++);

    return name;
}

void SessionManager::wireSession(TermSession& session)
{
    auto* raw = &session;

    session.view.onShellExit = [this, raw] { close(*raw); };

    session.view.onCwdChanged = [this](const std::string&) { persist(); };

    session.view.onNotify = [this, raw](const std::string& text)
    {
        raw->lastNotify = text;
        onNotify(*raw, text);
    };
}

TermSession& SessionManager::createSession(const std::string& name,
                                           const std::string& projectDir,
                                           const std::string& startCwd)
{
    auto& session = *sessions.emplace_back(std::make_unique<TermSession>(
        config, uniqueName(name), normalizedDir(projectDir), startCwd));
    wireSession(session);
    onSessionsChanged();
    return session;
}

TermSession& SessionManager::openProject(const std::string& dir)
{
    const auto path = normalizedDir(dir);

    if (auto* existing = find(path))
    {
        switchTo(*existing);
        return *existing;
    }

    auto& session = createSession(sessionNameFor(path), path, {});
    switchTo(session);
    return session;
}

TermSession& SessionManager::newSession(const std::string& dir)
{
    const auto path = normalizedDir(dir);
    auto& session = createSession(sessionNameFor(path), path, {});
    switchTo(session);
    return session;
}

void SessionManager::switchTo(TermSession& session)
{
    if (activeSession != &session)
        previousSession = activeSession;

    activeSession = &session;
    mru.touch(session.key());
    persist();
    onActiveChanged(session);
}

void SessionManager::switchToIndex(int index)
{
    if (index >= 0 && index < (int) sessions.size())
        switchTo(*sessions[(std::size_t) index]);
}

void SessionManager::switchToLast()
{
    if (previousSession != nullptr)
        for (auto& session: sessions)
            if (session.get() == previousSession)
            {
                switchTo(*previousSession);
                return;
            }
}

void SessionManager::close(TermSession& session)
{
    const auto index = std::find_if(sessions.begin(),
                                    sessions.end(),
                                    [&](auto& s) { return s.get() == &session; });

    if (index == sessions.end())
        return;

    auto closing = std::move(*index);
    sessions.erase(index);

    if (previousSession == closing.get())
        previousSession = nullptr;

    if (sessions.empty())
    {
        activeSession = nullptr;
        persist();
        onAllClosed();
        return;
    }

    if (activeSession == closing.get())
    {
        auto* next =
            previousSession != nullptr ? previousSession : sessions.back().get();
        previousSession = nullptr;
        activeSession = next;
        onActiveChanged(*next);
    }

    persist();
    onSessionsChanged();
}

void SessionManager::restoreOrCreateInitial()
{
    const auto& state = saved.peek();

    for (const auto& savedSession: state.sessions)
        createSession(savedSession.name, savedSession.projectDir, savedSession.cwd);

    if (sessions.empty())
        createSession("home", eacp::FilePath::homeDirectory().str(), {});

    const auto index = std::clamp(state.activeIndex, 0, (int) sessions.size() - 1);
    switchTo(*sessions[(std::size_t) index]);
}

void SessionManager::persist()
{
    saved.mutate(
        [&](SavedState& state)
        {
            state.sessions.clear();

            for (auto& session: sessions)
                state.sessions.push_back({session->name,
                                          session->projectDir,
                                          session->view.currentCwd()});

            state.activeIndex = 0;

            for (std::size_t i = 0; i < sessions.size(); ++i)
                if (sessions[i].get() == activeSession)
                    state.activeIndex = (int) i;
        });
}
} // namespace term
