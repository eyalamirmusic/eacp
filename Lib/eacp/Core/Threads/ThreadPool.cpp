#include "ThreadPool.h"
#include "EventLoop.h"
#include "Thread.h"
#include "../Platform/ScopedAutoReleasePool.h"
#include "../Utils/Singleton.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace eacp::Threads
{
struct TaskGroup::Tag
{
    int outstanding = 0;
    bool closing = false;
};

namespace
{
using ThreadPoolClock = std::chrono::steady_clock;
using ThreadPoolDeadline = std::optional<ThreadPoolClock::time_point>;
using ThreadPoolLock = std::unique_lock<std::mutex>;

struct ThreadPoolEntry
{
    PoolTask task;
    TaskGroup::Tag* group = nullptr;
    bool hasMainThreadCallbacks = false;
};

struct ThreadPoolCompletion
{
    Callback callback;
    TaskGroup::Tag* group = nullptr;
};

std::atomic<bool> threadPoolSharedCreated {false};

int threadPoolResolvedCount(int requested)
{
    return requested > 0 ? requested : ThreadPool::defaultThreadCount();
}

ThreadPoolDeadline threadPoolDeadlineAfter(Time::MS timeout)
{
    return ThreadPoolClock::now() + std::chrono::milliseconds(timeout.count);
}

bool threadPoolRunWork(const Callback& work)
{
    auto pool = ScopedAutoReleasePool {};

    try
    {
        work();
        return true;
    }
    catch (const std::exception& e)
    {
        LOG("ThreadPool: task threw: ", e.what());
    }
    catch (...)
    {
        LOG("ThreadPool: task threw an unknown exception");
    }

    return false;
}

PoolTask threadPoolTaskWithDefaults(PoolTask task)
{
    for (auto* callback: {&task.work, &task.onMainThread, &task.onCancelled})
        if (!*callback)
            *callback = [] {};

    return task;
}
} // namespace

struct ThreadPool::State : std::enable_shared_from_this<ThreadPool::State>
{
    explicit State(const ThreadPoolOptions& optionsToUse)
        : options(optionsToUse)
        , threadCount(threadPoolResolvedCount(optionsToUse.threadCount))
    {
    }

    struct FinishGuard
    {
        ~FinishGuard()
        {
            auto lock = ThreadPoolLock(state.mutex);
            state.finish(group);
        }

        State& state;
        TaskGroup::Tag* group;
    };

    // Not a thread_local: dyld never unloads an image holding one, and every
    // module linking eacp-core would then stay mapped after its last close.
    bool isCurrentThreadOurs() const
    {
        auto lock = ThreadPoolLock(mutex);
        return workerIds.contains(std::this_thread::get_id());
    }

    bool submit(ThreadPoolEntry entry)
    {
        auto lock = ThreadPoolLock(mutex);

        if (stopping || (entry.group != nullptr && entry.group->closing))
            return false;

        ++outstanding;

        auto wakesGroupWaiters = entry.group != nullptr;

        if (wakesGroupWaiters)
            ++entry.group->outstanding;

        queue.push_back(std::move(entry));

        auto needsThread =
            static_cast<int>(queue.size()) > idleThreads + startingThreads
            && reservedThreads < threadCount;

        auto index = reservedThreads + 1;

        if (needsThread)
        {
            ++reservedThreads;
            ++startingThreads;
        }

        lock.unlock();
        workAvailable.notify_one();

        if (wakesGroupWaiters)
            progress.notify_all();

        if (needsThread)
            startThread(index);

        return true;
    }

    // Unlocked: a realtime Thread's constructor waits for the new thread to
    // settle, and nothing else should wait behind that.
    void startThread(int index)
    {
        auto threadOptions = options.thread;

        if (!threadOptions.name.empty())
            threadOptions.name = Strings::concat(threadOptions.name, " ", index);

        auto thread = Thread {[this] { runWorker(); }, threadOptions};
        auto granted = thread.isRealtime();

        {
            auto lock = ThreadPoolLock(mutex);
            threads.add(std::move(thread));
            realtimeThreads += granted ? 1 : 0;
        }

        progress.notify_all();
    }

    void runWorker()
    {
        auto lock = ThreadPoolLock(mutex);
        --startingThreads;
        workerIds.add(std::this_thread::get_id());

        while (!exiting)
        {
            if (queue.empty())
            {
                ++idleThreads;
                workAvailable.wait(lock);
                --idleThreads;
                continue;
            }

            auto entry = std::move(queue.front());
            queue.pop_front();
            runEntry(lock, std::move(entry));
        }

        workerIds.removeAllMatches(std::this_thread::get_id());
    }

    // Nothing a task captured is destroyed with the lock held: its destructor
    // may call back into the pool.
    void runEntry(ThreadPoolLock& lock, ThreadPoolEntry entry)
    {
        lock.unlock();
        auto succeeded = threadPoolRunWork(entry.task.work);
        auto delivers = succeeded && entry.hasMainThreadCallbacks;
        auto delivery =
            ThreadPoolCompletion {std::move(entry.task.onMainThread), entry.group};
        entry = {};
        lock.lock();

        if (delivers && !stopping)
        {
            queueCompletion(lock, std::move(delivery));
            return;
        }

        lock.unlock();
        delivery.callback = nullptr;
        lock.lock();
        finish(delivery.group);
    }

    void finish(TaskGroup::Tag* group)
    {
        --outstanding;

        if (group != nullptr)
            --group->outstanding;

        progress.notify_all();
    }

    void queueCompletion(ThreadPoolLock& lock, ThreadPoolCompletion completion)
    {
        completions.push_back(std::move(completion));
        progress.notify_all();

        if (pokePending)
            return;

        pokePending = true;
        lock.unlock();
        callAsync([self = shared_from_this()] { self->deliverCompletions(); });
        lock.lock();
    }

    void deliverCompletions()
    {
        auto pool = ScopedAutoReleasePool {};

        {
            auto lock = ThreadPoolLock(mutex);
            pokePending = false;
        }

        while (auto completion = takeCompletion())
            runCompletion(std::move(*completion));
    }

    std::optional<ThreadPoolCompletion> takeCompletion()
    {
        auto lock = ThreadPoolLock(mutex);

        if (completions.empty())
            return std::nullopt;

        auto completion = std::move(completions.front());
        completions.pop_front();
        return completion;
    }

    void runCompletion(ThreadPoolCompletion completion)
    {
        auto guard = FinishGuard {*this, completion.group};
        completion.callback();
    }

    int cancel(TaskGroup::Tag* group)
    {
        auto removed = std::deque<ThreadPoolEntry> {};
        auto toDeliver = std::deque<ThreadPoolCompletion> {};
        auto lock = ThreadPoolLock(mutex);

        for (auto it = queue.begin(); it != queue.end();)
        {
            if (group != nullptr && it->group != group)
            {
                ++it;
                continue;
            }

            if (it->hasMainThreadCallbacks)
                toDeliver.push_back({std::move(it->task.onCancelled), it->group});
            else
                finish(it->group);

            removed.push_back(std::move(*it));
            it = queue.erase(it);
        }

        for (auto& completion: toDeliver)
            queueCompletion(lock, std::move(completion));

        return static_cast<int>(removed.size());
    }

    int& outstandingFor(TaskGroup::Tag* group)
    {
        return group != nullptr ? group->outstanding : outstanding;
    }

    std::optional<ThreadPoolEntry> takeQueued(TaskGroup::Tag* group)
    {
        for (auto it = queue.begin(); it != queue.end(); ++it)
        {
            if (it->group == group)
            {
                auto entry = std::move(*it);
                queue.erase(it);
                return entry;
            }
        }

        return std::nullopt;
    }

    bool wait(TaskGroup::Tag* group, ThreadPoolDeadline deadline)
    {
        auto onMainThread = isMainThread();
        auto onPoolThread = isCurrentThreadOurs();
        auto lock = ThreadPoolLock(mutex);

        while (outstandingFor(group) > 0)
        {
            if (deadline && ThreadPoolClock::now() >= *deadline)
                return false;

            if (onMainThread && !completions.empty())
            {
                auto completion = std::move(completions.front());
                completions.pop_front();
                lock.unlock();
                {
                    auto pool = ScopedAutoReleasePool {};
                    runCompletion(std::move(completion));
                }
                lock.lock();
                continue;
            }

            if (onPoolThread && group != nullptr)
            {
                if (auto entry = takeQueued(group))
                {
                    runEntry(lock, std::move(*entry));
                    continue;
                }
            }

            if (deadline)
                progress.wait_until(lock, *deadline);
            else
                progress.wait(lock);
        }

        return true;
    }

    // Moves what matches into `dropped`, which the caller destroys unlocked.
    template <typename Container, typename Predicate>
    void dropMatching(Container& container, Predicate&& matches, Container& dropped)
    {
        for (auto it = container.begin(); it != container.end();)
        {
            if (matches(*it))
            {
                finish(it->group);
                dropped.push_back(std::move(*it));
                it = container.erase(it);
            }
            else
                ++it;
        }
    }

    void closeGroup(TaskGroup::Tag* group)
    {
        auto droppedEntries = std::deque<ThreadPoolEntry> {};
        auto droppedCompletions = std::deque<ThreadPoolCompletion> {};
        auto lock = ThreadPoolLock(mutex);
        group->closing = true;

        auto inGroup = [group](const auto& item) { return item.group == group; };

        while (true)
        {
            dropMatching(queue, inGroup, droppedEntries);
            dropMatching(completions, inGroup, droppedCompletions);

            if (group->outstanding == 0)
                return;

            progress.wait(lock);
        }
    }

    void stop()
    {
        assert(!isCurrentThreadOurs() && "ThreadPool::stop from its own thread");

        if (isCurrentThreadOurs())
            return;

        auto stopLock = std::lock_guard(stopMutex);
        auto everything = [](const auto&) { return true; };
        auto joining = Vector<Thread> {};
        auto droppedEntries = std::deque<ThreadPoolEntry> {};
        auto droppedCompletions = std::deque<ThreadPoolCompletion> {};

        {
            auto lock = ThreadPoolLock(mutex);
            stopping = true;
            exiting = true;
            dropMatching(queue, everything, droppedEntries);
            dropMatching(completions, everything, droppedCompletions);
            progress.wait(lock,
                          [this] { return threads.size() == reservedThreads; });
            joining = std::move(threads);
            threads.clear();
            realtimeThreads = 0;
        }

        workAvailable.notify_all();
        droppedEntries.clear();
        droppedCompletions.clear();

        for (auto& thread: joining)
            thread.join();

        {
            auto lock = ThreadPoolLock(mutex);
            dropMatching(completions, everything, droppedCompletions);
            stopping = false;
            exiting = false;
            pokePending = false;
            idleThreads = 0;
            startingThreads = 0;
            reservedThreads = 0;
        }

        progress.notify_all();
    }

    const ThreadPoolOptions options;
    const int threadCount;

    mutable std::mutex mutex;
    std::mutex stopMutex;
    std::condition_variable workAvailable;
    std::condition_variable progress;
    std::deque<ThreadPoolEntry> queue;
    std::deque<ThreadPoolCompletion> completions;
    Vector<Thread> threads;
    Vector<std::thread::id> workerIds;
    int outstanding = 0;
    int idleThreads = 0;
    int reservedThreads = 0;
    int startingThreads = 0;
    int realtimeThreads = 0;
    bool pokePending = false;
    bool stopping = false;
    bool exiting = false;
};

ThreadPool::ThreadPool()
    : ThreadPool(ThreadPoolOptions {})
{
}

ThreadPool::ThreadPool(const ThreadPoolOptions& optionsToUse)
    : state(std::make_shared<State>(optionsToUse))
{
}

ThreadPool::~ThreadPool()
{
    stop();
}

ThreadPool& ThreadPool::shared()
{
    auto& pool = Singleton::get<ThreadPool>();
    threadPoolSharedCreated = true;
    return pool;
}

int ThreadPool::defaultThreadCount()
{
    auto cores = static_cast<int>(std::thread::hardware_concurrency());
    return cores == 0 ? 2 : std::max(cores, 1);
}

bool ThreadPool::submit(Callback work)
{
    auto task = PoolTask {.work = std::move(work)};
    return state->submit(
        {threadPoolTaskWithDefaults(std::move(task)), nullptr, false});
}

bool ThreadPool::submit(PoolTask task)
{
    return state->submit(
        {threadPoolTaskWithDefaults(std::move(task)), nullptr, true});
}

int ThreadPool::cancelPending()
{
    return state->cancel(nullptr);
}

void ThreadPool::waitUntilIdle()
{
    assert(!isPoolThread() && "ThreadPool::waitUntilIdle from its own thread");

    if (!isPoolThread())
        state->wait(nullptr, std::nullopt);
}

bool ThreadPool::waitUntilIdleFor(Time::MS timeout)
{
    assert(!isPoolThread() && "ThreadPool::waitUntilIdleFor from its own thread");

    if (isPoolThread())
        return false;

    return state->wait(nullptr, threadPoolDeadlineAfter(timeout));
}

bool ThreadPool::isIdle() const
{
    auto lock = ThreadPoolLock(state->mutex);
    return state->outstanding == 0;
}

void ThreadPool::stop()
{
    state->stop();
}

int ThreadPool::threadCount() const
{
    return state->threadCount;
}

int ThreadPool::startedThreadCount() const
{
    auto lock = ThreadPoolLock(state->mutex);
    return state->threads.size();
}

int ThreadPool::realtimeThreadCount() const
{
    auto lock = ThreadPoolLock(state->mutex);
    return state->realtimeThreads;
}

bool ThreadPool::isPoolThread() const
{
    return state->isCurrentThreadOurs();
}

const ThreadPoolOptions& ThreadPool::options() const
{
    return state->options;
}

TaskGroup::TaskGroup()
    : TaskGroup(ThreadPool::shared())
{
}

TaskGroup::TaskGroup(ThreadPool& poolToUse)
    : pool(poolToUse)
    , tag(new Tag {})
{
}

TaskGroup::~TaskGroup()
{
    pool.state->closeGroup(tag.get());
}

bool TaskGroup::submit(Callback work)
{
    auto task = PoolTask {.work = std::move(work)};
    return pool.state->submit(
        {threadPoolTaskWithDefaults(std::move(task)), tag.get(), false});
}

bool TaskGroup::submit(PoolTask task)
{
    return pool.state->submit(
        {threadPoolTaskWithDefaults(std::move(task)), tag.get(), true});
}

int TaskGroup::cancelPending()
{
    return pool.state->cancel(tag.get());
}

void TaskGroup::wait()
{
    pool.state->wait(tag.get(), std::nullopt);
}

bool TaskGroup::waitFor(Time::MS timeout)
{
    return pool.state->wait(tag.get(), threadPoolDeadlineAfter(timeout));
}

bool TaskGroup::isIdle() const
{
    auto lock = ThreadPoolLock(pool.state->mutex);
    return tag->outstanding == 0;
}

void stopSharedThreadPool()
{
    if (threadPoolSharedCreated)
        ThreadPool::shared().stop();
}
} // namespace eacp::Threads
