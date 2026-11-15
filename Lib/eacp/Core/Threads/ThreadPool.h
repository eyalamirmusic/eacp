#pragma once

#include "Thread.h"

namespace eacp::Threads
{
struct ThreadPoolOptions
{
    // 0 is ThreadPool::defaultThreadCount(), one per logical core.
    int threadCount = 0;

    // Each thread takes these, with " <index>" appended to the name.
    ThreadOptions thread {.name = "EACP Pool"};
};

struct PoolTask
{
    Callback work = [] {};
    Callback onMainThread = [] {};

    // Runs on the main thread instead of both when cancelled before starting.
    Callback onCancelled = [] {};
};

// Threads start lazily, one per submit that finds none idle, up to threadCount.
// A task is outstanding until its work returned and its main-thread callback
// ran or was dropped; a wait on the main thread delivers those callbacks itself.
class ThreadPool
{
public:
    ThreadPool();
    explicit ThreadPool(const ThreadPoolOptions& optionsToUse);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    static ThreadPool& shared();
    static int defaultThreadCount();

    // False when the pool is stopping: the task is dropped and nothing runs.
    bool submit(Callback work);
    bool submit(PoolTask task);

    int cancelPending();

    // Not from a thread of this pool: asserts, and the timed form returns false.
    void waitUntilIdle();
    bool waitUntilIdleFor(Time::MS timeout);
    bool isIdle() const;

    // Cancels what is queued without callbacks, joins the threads and drops
    // undelivered main-thread callbacks; the pool accepts work again after.
    void stop();

    int threadCount() const;
    int startedThreadCount() const;
    [[nodiscard]] int realtimeThreadCount() const;
    bool isPoolThread() const;
    const ThreadPoolOptions& options() const;

    struct State;

private:
    friend class TaskGroup;

    std::shared_ptr<State> state;
};

// A wait on a thread of the pool runs the group's queued tasks meanwhile.
// Destruction cancels what has not started, waits for what is running and
// drops the group's undelivered main-thread callbacks.
class TaskGroup
{
public:
    TaskGroup();
    explicit TaskGroup(ThreadPool& poolToUse);
    ~TaskGroup();

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    bool submit(Callback work);
    bool submit(PoolTask task);

    int cancelPending();
    void wait();
    bool waitFor(Time::MS timeout);
    bool isIdle() const;

    struct Tag;

private:
    ThreadPool& pool;
    OwningPointer<Tag> tag;
};

// Stops ThreadPool::shared() if anything ever created it. Called by Apps::run
// after the app is destroyed and by detachCurrentThreadAsMain. Internal to eacp.
void stopSharedThreadPool();
} // namespace eacp::Threads
