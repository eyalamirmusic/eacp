#pragma once

#include "../Utils/Time.h"

#include <mutex>
#include <condition_variable>

namespace eacp::Threads
{
class BinarySemaphore
{
public:
    void release();

    void acquire();

    // False when the timeout passed before a release.
    bool tryAcquireFor(Time::MS timeout);

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

    // False when the timeout passed before a signal.
    bool waitFor(Time::MS timeout);

private:
    BinarySemaphore semaphore;
};
} // namespace eacp::Threads