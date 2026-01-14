#pragma once

#include <memory>
#include <semaphore>

namespace eacp::Threads
{
class TaskSemaphore
{
public:
    void signal();
    void wait();

private:
    std::binary_semaphore semaphore {0};
};

} // namespace eacp::Threads
