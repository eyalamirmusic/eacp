#include "TaskSemaphore.h"

namespace eacp::Threads
{
void TaskSemaphore::wait()
{
    semaphore.acquire();
}

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

void TaskSemaphore::signal()
{
    semaphore.release();
}
} // namespace eacp::Threads