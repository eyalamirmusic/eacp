#include "Thread.h"

#include <atomic>
#include <future>
#include <thread>

namespace eacp::Threads
{
using AtomicId = std::atomic<std::thread::id>;

namespace
{
bool threadApplyOptions(const ThreadOptions& options)
{
    if (!options.name.empty())
        setCurrentThreadName(options.name);

    if (options.realtime)
        return setCurrentThreadRealtime(*options.realtime);

    setCurrentThreadPriority(options.priority);
    return false;
}
} // namespace

struct Thread::Native
{
    void takeOver(Native& other)
    {
        thread.swap(other.thread);
        id.swap(other.id);
        std::swap(options, other.options);
        realtime = other.realtime.exchange(realtime.load());
    }

    std::thread thread;
    std::shared_ptr<AtomicId> id = std::make_shared<AtomicId>();
    ThreadOptions options;
    std::atomic<bool> realtime {false};
};

Thread::Thread(const ThreadOptions& optionsToUse)
{
    setOptions(optionsToUse);
}

Thread::Thread(const Callback& body, const ThreadOptions& optionsToUse)
{
    setOptions(optionsToUse);
    start(body);
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
    join();

    auto id = std::make_shared<AtomicId>();
    auto settled = std::make_shared<std::promise<bool>>();
    auto outcome = settled->get_future();

    auto run = [id, body, settled, options = impl->options]
    {
        id->store(std::this_thread::get_id());
        settled->set_value(threadApplyOptions(options));
        body();

        if (options.realtime)
            endCurrentThreadRealtime();
    };

    impl->id = id;
    impl->thread = std::thread {run};

    if (impl->options.realtime)
        impl->realtime = outcome.get();
}

void Thread::join()
{
    if (isJoinable())
    {
        assert(!isCurrentThread());
        impl->thread.join();
    }

    impl->realtime = false;
}

void Thread::setOptions(const ThreadOptions& optionsToUse)
{
    impl->options = optionsToUse;
}

const ThreadOptions& Thread::options() const
{
    return impl->options;
}

bool Thread::isRealtime() const
{
    return impl->realtime;
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
