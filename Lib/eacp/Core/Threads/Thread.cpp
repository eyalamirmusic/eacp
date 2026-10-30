#include "Thread.h"

#include <atomic>
#include <thread>

namespace eacp::Threads
{
using AtomicId = std::atomic<std::thread::id>;

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

    auto run = [id, body]
    {
        id->store(std::this_thread::get_id());
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
