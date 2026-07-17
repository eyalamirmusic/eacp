#pragma once

#include "Config.h"
#include "MruStore.h"
#include "TerminalView.h"

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

    MIRO_REFLECT(name, projectDir, cwd)
};

struct SavedState
{
    std::vector<SavedSession> sessions;
    int activeIndex = 0;

    MIRO_REFLECT(sessions, activeIndex)
};

// One named shell bound to a project directory.
class TermSession
{
public:
    TermSession(const AppConfig& config,
                std::string nameToUse,
                std::string projectDirToUse,
                const std::string& startCwd);

    // Stable identity for MRU stamps and notification routing.
    const std::string& key() const
    {
        return projectDir.empty() ? name : projectDir;
    }

    // True when a Claude Code conversation owns this session's terminal.
    bool isClaude() const;

    std::string name;
    std::string projectDir;
    std::string lastNotify;
    TerminalView view;
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
    const std::vector<std::unique_ptr<TermSession>>& all() const
    {
        return sessions;
    }

    TermSession* find(const std::string& key);
    TermSession& openProject(const std::string& dir);

    // Always creates a fresh session (openProject dedupes by directory).
    TermSession& newSession(const std::string& dir);
    void switchTo(TermSession& session);
    void switchToIndex(int index);
    void switchToLast();
    void close(TermSession& session);
    void restoreOrCreateInitial();

    std::int64_t lastUsed(const std::string& key)
    {
        return mru.lastUsed(key);
    }

    std::function<void(TermSession&)> onActiveChanged = [](TermSession&) {};
    std::function<void(TermSession&, const std::string&)> onNotify =
        [](TermSession&, const std::string&) {};
    eacp::Callback onSessionsChanged = [] {};
    eacp::Callback onAllClosed = [] {};

private:
    TermSession& createSession(const std::string& name,
                               const std::string& projectDir,
                               const std::string& startCwd);
    void wireSession(TermSession& session);
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
