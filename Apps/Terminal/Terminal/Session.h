#pragma once

#include "Config.h"
#include "MruStore.h"
#include "SessionView.h"

#include <Miro/Reflect.h>
#include <emberstore/Emberstore.h>

#include <memory>
#include <string>
#include <vector>

namespace term
{
struct SavedSession
{
    std::string name;
    std::string projectDir;
    std::string cwd;
    std::vector<SavedPane> panes;

    MIRO_REFLECT(name, projectDir, cwd, panes)
};

struct SavedState
{
    std::vector<SavedSession> sessions;
    int activeIndex = 0;

    MIRO_REFLECT(sessions, activeIndex)
};

// One named pane tree bound to a project directory.
class TermSession
{
public:
    TermSession(const AppConfig& config,
                std::string nameToUse,
                std::string projectDirToUse,
                const std::string& startCwd);

    // Stable identity for MRU stamps and notification routing.
    const std::string& key() const { return projectDir.empty() ? name : projectDir; }

    // True when a Claude Code conversation owns any pane of this session.
    bool isClaude() const { return view.isClaudeAnywhere(); }

    // The active pane's title / directory; empty when the session has no
    // panes (mid-teardown).
    std::string activeTitle() const;
    std::string activeWorkingDirectory() const;

    std::string name;
    std::string projectDir;
    std::string lastNotify;
    SessionView view;
};

// Owns every open session plus the recency store, and persists the open set
// (emberstore) so a relaunch brings the workspace back: shells respawn in
// their last cwd. Running processes don't survive a quit — that's the
// daemon's job, later.
class SessionManager
{
public:
    explicit SessionManager(const AppConfig& configToUse);

    TermSession* active() { return activeSession; }
    const std::vector<std::unique_ptr<TermSession>>& all() const { return sessions; }

    TermSession* find(const std::string& key);
    TermSession& openProject(const std::string& dir);

    // Always creates a fresh session (openProject dedupes by directory).
    TermSession& newSession(const std::string& dir);
    void switchTo(TermSession& session);
    void switchToIndex(int index);
    void switchToLast();
    void close(TermSession& session);
    void restoreOrCreateInitial();

    std::int64_t lastUsed(const std::string& key) { return mru.lastUsed(key); }

    std::function<void(TermSession&)> onActiveChanged = [](TermSession&) {};
    std::function<void(TermSession&, const std::string&)> onNotify =
        [](TermSession&, const std::string&) {};
    eacp::Callback onSessionsChanged = [] {};
    eacp::Callback onAllClosed = [] {};

    // Fired for every pane the session creates, so the app shell installs
    // per-pane hooks (interceptKey, title tracking).
    std::function<void(TermSession&, TerminalView&)> onPaneWired =
        [](TermSession&, TerminalView&) {};

    void persistNow() { persist(); }

private:
    TermSession& createSession(const std::string& name,
                               const std::string& projectDir,
                               const std::string& startCwd,
                               const std::vector<SavedPane>& panes);
    void wireSession(TermSession& session);
    void closeIfPresent(TermSession* session);
    std::string uniqueName(const std::string& base) const;
    void persist();

    const AppConfig& config;
    emberstore::Database db;
    MruStore mru;
    emberstore::Document<SavedState> saved;
    std::vector<std::unique_ptr<TermSession>> sessions;
    TermSession* activeSession = nullptr;
    TermSession* previousSession = nullptr;
};
} // namespace term
