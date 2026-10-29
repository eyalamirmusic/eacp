#pragma once

#include "../Utils/Common.h"
#include "../Utils/Time.h"

#include <functional>
#include <string>
#include <string_view>

namespace eacp::Threads
{
// A hang watch over one thread, opened by whatever watches threads (the crash
// reporter installs one with setThreadWatchFactory) for a Thread whose options
// name a watch. Core only holds it; it knows nothing of what watches.
struct ThreadWatchScope
{
    virtual ~ThreadWatchScope() = default;

    virtual void sendHeartbeat() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
};

using ThreadWatchFactory = std::function<OwningPointer<ThreadWatchScope>(
    std::string_view name, Time::MS threshold)>;

struct ThreadOptions
{
    // Empty is unwatched. Otherwise the body runs inside a watch of this name,
    // which it keeps alive with sendThreadHeartbeat().
    std::string watchName;

    // Zero is the watcher's own default.
    Time::MS watchThreshold {};
};

// Applies to threads started after it; an empty factory opens no watches.
void setThreadWatchFactory(const ThreadWatchFactory& factory);

// On a Thread whose options opened a watch: send it a heartbeat, or pause and resume it
// around work meant to outlast its threshold. No-ops on any other thread.
void sendThreadHeartbeat();
void pauseThreadWatch();
void resumeThreadWatch();

class Thread
{
public:
    Thread();
    explicit Thread(const Callback& body);
    Thread(const ThreadOptions& options, const Callback& body);
    ~Thread();

    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    void start(const Callback& body);
    void start(const ThreadOptions& options, const Callback& body);
    void join();

    [[nodiscard]] bool isJoinable() const;
    [[nodiscard]] bool isCurrentThread() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Threads
