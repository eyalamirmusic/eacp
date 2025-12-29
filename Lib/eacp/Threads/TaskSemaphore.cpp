#include "TaskSemaphore.h"

namespace eacp::Threads
{
void TaskSemaphore::wait()
{
    semaphore.acquire();
}

void TaskSemaphore::signal()
{
    semaphore.release();
}

} // namespace eacp::Threads