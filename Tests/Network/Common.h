#pragma once

#include <eacp/Network/Network.h>
#include <NanoTest/NanoTest.h>

#include <condition_variable>
#include <mutex>

// A server handler that has to still be in flight when the client gives up
// blocks here instead of sleeping for a fixed span. The test releases it once
// it has asserted, so teardown never waits out the tail of a sleep that has
// already served its purpose - which is where these suites used to spend most
// of their time. The bound on wait() is a backstop against a test that fails
// before releasing, not a timing the tests rely on.
struct StallGate
{
    ~StallGate() { release(); }

    void wait()
    {
        auto lock = std::unique_lock(mutex);
        opened.wait_for(lock, std::chrono::seconds(10), [this] { return isOpen; });
    }

    void release()
    {
        {
            auto lock = std::scoped_lock(mutex);
            isOpen = true;
        }

        opened.notify_all();
    }

    std::mutex mutex;
    std::condition_variable opened;
    bool isOpen = false;
};
