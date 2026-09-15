#pragma once

#include <eacp/Network/Network.h>
#include <NanoTest/NanoTest.h>

#include <condition_variable>
#include <mutex>

// A bound for calls that must not wait for a server that is deliberately
// stalled - the client's own timeout, or the fact that a request is
// asynchronous, is what has to end them. The stalled handlers never answer
// while the assertions run, so any completion at all already proves that; the
// bound only has to sit clear of the stalls a shared runner inflicts on every
// process at once. Windows CI showed how far that reaches: in one burst a
// pure-logic test went from 0.01s to 0.36s and a 250ms-limited request from
// 0.30s to just over a second, which is where the previous 1000ms bound
// failed.
constexpr auto withoutWaitingForTheServer = eacp::Time::MS {3000};

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
