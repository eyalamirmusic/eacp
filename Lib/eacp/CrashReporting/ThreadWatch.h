#pragma once

#include <eacp/Core/Threads/Thread.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

// A worker thread watched for hangs the way the main thread is: open one on
// the thread, beat it from the thread's loop, and a silence past its threshold
// is sampled and reported as a hang, once per episode, with the app left
// running.
//
//   auto watch = CrashReporting::ThreadWatch {"encoder", 5s};
//   while (running)
//   {
//       encodeNextFrame();
//       watch.beat();
//   }
//
// A Threads::Thread started with ThreadOptions::watchName opens one around its
// body, beaten with Threads::beatThreadWatch(). Only opted-in threads are
// watched, at most Detail::maxWatchedThreads - 1 at once beside the main one;
// a watch opened before start(), after stop(), with detection off or with
// every slot taken watches nothing.
namespace eacp::CrashReporting
{
class ThreadWatch final : public Threads::ThreadWatchScope
{
public:
    // A zero threshold is Options::hangThreshold.
    explicit ThreadWatch(std::string_view name,
                         std::chrono::milliseconds threshold = {});
    ~ThreadWatch() override;

    ThreadWatch(const ThreadWatch&) = delete;
    ThreadWatch& operator=(const ThreadWatch&) = delete;

    // From the thread that opened it.
    void beat() override;

    // Around work meant to outlast the threshold. Nests.
    void pause() override;
    void resume() override;

    bool isWatched() const;

private:
    std::optional<std::size_t> slot;
};
} // namespace eacp::CrashReporting
