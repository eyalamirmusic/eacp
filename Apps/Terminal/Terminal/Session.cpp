#include "Session.h"
#include "Projects.h"

#include <eacp/Core/Threads/Async.h>
#include <emberstore/AppDatabase.h>

#include <algorithm>

namespace term
{
namespace
{
constexpr auto persistDelay = eacp::Time::MS {500};

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

std::string TermSession::activeTitle() const
{
    const auto* pane = view.activePane();
    return pane != nullptr ? pane->currentTitle() : std::string {};
}

std::string TermSession::activeWorkingDirectory() const
{
    const auto* pane = view.activePane();
    return pane != nullptr ? pane->workingDirectory() : projectDir;
}

SessionManager::SessionManager(const AppConfig& configToUse)
    : config(configToUse)
    , db(emberstore::databaseForApp(
          "tamber", "cowterm", emberstore::Durability::Atomic))
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

    session.view.onPaneCreated = [this, raw](TerminalView& pane)
    {
        pane.onNotify = [this, raw](const std::string& text)
        {
            raw->lastNotify = text;
            onNotify(*raw, text);
        };

        pane.onCwdChanged = [this](const std::string&) { persist(); };

        onPaneWired(*raw, pane);
    };

    // The last pane closing ends the session. Deferred: onEmpty fires from
    // inside the session view, and close() destroys it.
    session.view.onEmpty = [this, raw]
    { eacp::Threads::callAsync([this, raw] { closeIfPresent(raw); }); };

    session.view.onActivePaneChanged = [this, raw]
    {
        if (activeSession == raw)
            onActiveChanged(*raw);

        persist();
    };
}

TermSession& SessionManager::createSession(const std::string& name,
                                           const std::string& projectDir,
                                           const std::string& startCwd,
                                           const std::vector<SavedPane>& panes)
{
    auto& session = *sessions.emplace_back(std::make_unique<TermSession>(
        config, uniqueName(name), normalizedDir(projectDir), startCwd));
    wireSession(session);
    session.view.restore(panes);
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

    auto& session = createSession(sessionNameFor(path), path, {}, {});
    switchTo(session);
    return session;
}

TermSession& SessionManager::newSession(const std::string& dir)
{
    const auto path = normalizedDir(dir);
    auto& session = createSession(sessionNameFor(path), path, {}, {});
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

void SessionManager::closeIfPresent(TermSession* session)
{
    for (auto& candidate: sessions)
        if (candidate.get() == session)
        {
            close(*candidate);
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

    // An explicit session close kills its shells; app teardown, which
    // never comes through here, only detaches so they survive in the
    // daemon.
    session.view.terminateAll();

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
        createSession(savedSession.name,
                      savedSession.projectDir,
                      savedSession.cwd,
                      savedSession.panes);

    if (sessions.empty())
        createSession("home", eacp::FilePath::homeDirectory().str(), {}, {});

    const auto index = std::clamp(state.activeIndex, 0, (int) sessions.size() - 1);
    switchTo(*sessions[(std::size_t) index]);
}

SessionManager::~SessionManager()
{
    *alive = false;

    if (persistPending)
        writeState();
}

void SessionManager::persist()
{
    if (persistPending)
        return;

    persistPending = true;

    eacp::Threads::delay(persistDelay)
        .then(
            [this, guard = std::weak_ptr<bool> {alive}]
            {
                if (guard.expired() || !persistPending)
                    return;

                persistPending = false;
                writeState();
            });
}

void SessionManager::persistNow()
{
    persistPending = false;
    writeState();
}

void SessionManager::writeState()
{
    saved.mutate(
        [&](SavedState& state)
        {
            state.sessions.clear();

            for (auto& session: sessions)
                state.sessions.push_back({session->name,
                                          session->projectDir,
                                          session->activeWorkingDirectory(),
                                          session->view.snapshot()});

            state.activeIndex = 0;

            for (std::size_t i = 0; i < sessions.size(); ++i)
                if (sessions[i].get() == activeSession)
                    state.activeIndex = (int) i;
        });
}
} // namespace term
