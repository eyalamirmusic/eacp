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

    // Returns false if the timeout elapsed before the signal arrived, so a
    // caller waiting on work it does not control can give up instead of
    // blocking forever. A timeout of zero or less waits indefinitely.
    bool acquireFor(Time::MS timeout);

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
    bool waitFor(Time::MS timeout);

private:
    BinarySemaphore semaphore;
};
} // namespace eacp::Threads