#include "Thread.h"

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace eacp::Threads
{
using AtomicId = std::atomic<std::thread::id>;

namespace
{
// Keyed by thread rather than thread_local: macOS never unloads an image
// that has thread-local variables, and plugins link Core.
struct ThreadWatchRegistry
{
    std::mutex mutex;
    ThreadWatchFactory factory;
    std::map<std::thread::id, std::reference_wrapper<ThreadWatchScope>> openWatches;
};

ThreadWatchRegistry& getThreadWatchRegistry()
{
    static auto registry = ThreadWatchRegistry {};
    return registry;
}

std::unique_ptr<ThreadWatchScope> openThreadWatch(const ThreadOptions& options)
{
    if (options.watchName.empty())
        return {};

    auto& registry = getThreadWatchRegistry();
    auto lock = std::lock_guard {registry.mutex};

    if (!registry.factory)
        return {};

    return registry.factory(options.watchName, options.watchThreshold);
}

struct CurrentThreadWatch
{
    explicit CurrentThreadWatch(const ThreadOptions& options)
        : watch(openThreadWatch(options))
    {
        if (!watch)
            return;

        auto& registry = getThreadWatchRegistry();
        auto lock = std::lock_guard {registry.mutex};
        registry.openWatches.insert_or_assign(std::this_thread::get_id(), *watch);
    }

    ~CurrentThreadWatch()
    {
        auto& registry = getThreadWatchRegistry();
        auto lock = std::lock_guard {registry.mutex};
        registry.openWatches.erase(std::this_thread::get_id());
    }

    std::unique_ptr<ThreadWatchScope> watch;
};

template <typename WatchFunction>
void applyToCurrentThreadWatch(WatchFunction apply)
{
    auto& registry = getThreadWatchRegistry();
    auto lock = std::lock_guard {registry.mutex};
    auto found = registry.openWatches.find(std::this_thread::get_id());

    if (found != registry.openWatches.end())
        apply(found->second.get());
}
} // namespace

void setThreadWatchFactory(const ThreadWatchFactory& factory)
{
    auto& registry = getThreadWatchRegistry();
    auto lock = std::lock_guard {registry.mutex};
    registry.factory = factory;
}

void beatThreadWatch()
{
    applyToCurrentThreadWatch([](ThreadWatchScope& watch) { watch.beat(); });
}

void pauseThreadWatch()
{
    applyToCurrentThreadWatch([](ThreadWatchScope& watch) { watch.pause(); });
}

void resumeThreadWatch()
{
    applyToCurrentThreadWatch([](ThreadWatchScope& watch) { watch.resume(); });
}

struct Thread::Native
{
    void takeOver(Native& other)
    {
        thread.swap(other.thread);
        id.swap(other.id);
    }

    std::thread thread;
    std::shared_ptr<AtomicId> id = std::make_shared<AtomicId>();
};

Thread::Thread() = default;

Thread::Thread(const Callback& body)
{
    start(body);
}

Thread::Thread(const ThreadOptions& options, const Callback& body)
{
    start(options, body);
}

Thread::~Thread()
{
    join();
}

Thread::Thread(Thread&& other) noexcept
{
    impl->takeOver(*other.impl);
}

Thread& Thread::operator=(Thread&& other) noexcept
{
    if (this != &other)
    {
        join();
        impl->takeOver(*other.impl);
    }

    return *this;
}

void Thread::start(const Callback& body)
{
    start(ThreadOptions {}, body);
}

void Thread::start(const ThreadOptions& options, const Callback& body)
{
    join();

    auto id = std::make_shared<AtomicId>();

    auto run = [id, body, options]
    {
        id->store(std::this_thread::get_id());
        auto watch = CurrentThreadWatch {options};
        body();
    };

    impl->id = id;
    impl->thread = std::thread {run};
}

void Thread::join()
{
    if (!isJoinable())
        return;

    assert(!isCurrentThread());
    impl->thread.join();
}

bool Thread::isJoinable() const
{
    return impl->thread.joinable();
}

bool Thread::isCurrentThread() const
{
    return impl->id->load() == std::this_thread::get_id();
}
} // namespace eacp::Threads
