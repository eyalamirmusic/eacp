#pragma once

#include <eacp/Core/Utils/Time.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

// Every blocking wait in the thread tests gives up after this, so a bug fails
// rather than hangs; a passing run never waits on it.
inline constexpr auto threadTestBound = eacp::Time::MS {30000};

// One-shot latch for the thread tests.
class Gate
{
public:
    void open()
    {
        {
            auto lock = std::lock_guard(mutex);
            isOpen = true;
        }

        opened.notify_all();
    }

    bool wait()
    {
        auto lock = std::unique_lock(mutex);
        return opened.wait_for(lock,
                               std::chrono::milliseconds(threadTestBound.count),
                               [this] { return isOpen; });
    }

private:
    std::mutex mutex;
    std::condition_variable opened;
    bool isOpen = false;
};
