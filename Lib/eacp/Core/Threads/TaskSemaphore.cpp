#include "TaskSemaphore.h"

#include <chrono>

namespace eacp::Threads
{
void BinarySemaphore::release()
{
    auto lock = std::unique_lock(mtx);
    count = true;
    cv.notify_one();
}

void BinarySemaphore::acquire()
{
    auto lock = std::unique_lock(mtx);
    cv.wait(lock, [this] { return count; });
    count = false;
}

bool BinarySemaphore::tryAcquireFor(Time::MS timeout)
{
    auto lock = std::unique_lock(mtx);
    auto released = [this] { return count; };

    if (!cv.wait_for(lock, std::chrono::milliseconds(timeout.count), released))
        return false;

    count = false;
    return true;
}

void TaskSemaphore::signal()
{
    semaphore.release();
}

void TaskSemaphore::wait()
{
    semaphore.acquire();
}

bool TaskSemaphore::waitFor(Time::MS timeout)
{
    return semaphore.tryAcquireFor(timeout);
}
} // namespace eacp::Threads
