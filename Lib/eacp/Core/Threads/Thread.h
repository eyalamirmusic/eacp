#pragma once

#include "ThreadUtils.h"
#include "../Utils/Common.h"

namespace eacp::Threads
{
// Applied on the new thread before its body runs; an empty name leaves it
// unnamed. `realtime` replaces `priority`, falling back to High if refused.
struct ThreadOptions
{
    std::string name = "EACP Thread";
    ThreadPriority priority = ThreadPriority::Normal;
    std::optional<RealtimeOptions> realtime = std::nullopt;
};

class Thread
{
public:
    explicit Thread(const ThreadOptions& optionsToUse = {});
    explicit Thread(const Callback& body, const ThreadOptions& optionsToUse = {});
    ~Thread();

    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    void start(const Callback& body);
    void join();

    // Takes effect at the next start().
    void setOptions(const ThreadOptions& optionsToUse);
    const ThreadOptions& options() const;

    // Whether the running thread was granted realtime; settled when start()
    // returns, false again after join().
    [[nodiscard]] bool isRealtime() const;

    [[nodiscard]] bool isJoinable() const;
    [[nodiscard]] bool isCurrentThread() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::Threads
