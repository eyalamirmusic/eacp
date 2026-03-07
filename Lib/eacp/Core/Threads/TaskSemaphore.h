#pragma once

#include <mutex>
#include <condition_variable>

namespace eacp::Threads
{
class BinarySemaphore
{
public:
    void release();
    void acquire();

private:
    std::mutex mtx;
    std::condition_variable cv;
    bool count = false;
};


class TaskSemaphore
{
public:
    void signal();
    void wait();

private:
    BinarySemaphore semaphore;
};
} // namespace eacp::Threads