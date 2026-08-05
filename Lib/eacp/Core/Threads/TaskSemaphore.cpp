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

bool BinarySemaphore::acquireFor(Time::MS timeout)
{
    if (timeout.count <= 0)
    {
        acquire();
        return true;
    }

    auto lock = std::unique_lock(mtx);
    auto duration = std::chrono::milliseconds(timeout.count);

    if (!cv.wait_for(lock, duration, [this] { return count; }))
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
    return semaphore.acquireFor(timeout);
}
} // namespace eacp::Threads