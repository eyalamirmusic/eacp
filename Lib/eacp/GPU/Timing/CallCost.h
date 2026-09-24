#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace eacp::GPU
{
// What the CPU has spent inside one kind of call into the driver - creating a
// buffer, waiting on a fence - summed over every call so far. The kind of cost
// a GPU timing cannot show: a quarter of a millisecond at a time, on the CPU,
// spread across a whole run.
struct CallCost
{
    std::string label;
    int calls = 0;
    double seconds = 0.0;
    std::int64_t bytes = 0;

    double meanMicroseconds() const
    {
        return calls > 0 ? seconds * 1e6 / (double) calls : 0.0;
    }
};

// One kind of call's running total. Declared where the call is made, usually
// as a function-local static, and timed with a ScopedCallCost around the call:
//
//     static auto creations = CallCostCounter {"buffers"};
//     auto cost = ScopedCallCost {creations, bytes};
//     device->CreateCommittedResource(...);
//
// Every counter alive is listed by callCosts(), so an app reads the totals
// where it prints its other timings; nothing is printed on its own.
class CallCostCounter
{
public:
    explicit CallCostCounter(std::string label);
    ~CallCostCounter();

    CallCostCounter(const CallCostCounter&) = delete;
    CallCostCounter& operator=(const CallCostCounter&) = delete;

    void record(double seconds, std::int64_t bytes = 0);
    CallCost total() const;

private:
    mutable std::mutex mutex;
    CallCost running;
};

// Times the scope it lives in into a counter: construct it before the call,
// and its destructor records how long the call took and how many bytes it
// moved.
class ScopedCallCost
{
public:
    explicit ScopedCallCost(CallCostCounter& counterToUse,
                            std::int64_t bytesToUse = 0)
        : counter(counterToUse)
        , bytes(bytesToUse)
    {
    }

    ~ScopedCallCost()
    {
        auto elapsed = std::chrono::steady_clock::now() - startedAt;
        counter.record(std::chrono::duration<double>(elapsed).count(), bytes);
    }

    ScopedCallCost(const ScopedCallCost&) = delete;
    ScopedCallCost& operator=(const ScopedCallCost&) = delete;

private:
    CallCostCounter& counter;
    std::int64_t bytes;
    std::chrono::steady_clock::time_point startedAt =
        std::chrono::steady_clock::now();
};

// The total of every counter alive in the process, in the order they were
// made. Empty on a backend that counts nothing.
Vector<CallCost> callCosts();
} // namespace eacp::GPU
