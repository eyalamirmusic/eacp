#include "Thread.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

namespace eacp::Threads
{
using AtomicId = std::atomic<std::thread::id>;

namespace
{
struct ThreadWatchRegistry
{
    std::mutex mutex;
    ThreadWatchFactory factory;
};

ThreadWatchRegistry& getThreadWatchRegistry()
{
    static auto registry = ThreadWatchRegistry {};
    return registry;
}

thread_local auto currentThreadWatch =
    std::optional<std::reference_wrapper<ThreadWatchScope>> {};

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
        if (watch)
            currentThreadWatch = *watch;
    }

    ~CurrentThreadWatch() { currentThreadWatch.reset(); }

    std::unique_ptr<ThreadWatchScope> watch;
};
} // namespace

void setThreadWatchFactory(const ThreadWatchFactory& factory)
{
    auto& registry = getThreadWatchRegistry();
    auto lock = std::lock_guard {registry.mutex};
    registry.factory = factory;
}

void beatThreadWatch()
{
    if (currentThreadWatch)
        currentThreadWatch->get().beat();
}

void pauseThreadWatch()
{
    if (currentThreadWatch)
        currentThreadWatch->get().pause();
}

void resumeThreadWatch()
{
    if (currentThreadWatch)
        currentThreadWatch->get().resume();
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
