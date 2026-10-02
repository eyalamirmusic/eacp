#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace eacp::GPU
{
// What a D3D12 build pays and a Metal one does not: a committed resource per
// buffer, and a CPU block per fence. Both are invisible in a phase timing -
// they are spread a quarter of a millisecond at a time - so they are counted
// here instead. EACP_BUFFER_STATS=1 prints each counter's total at exit.
class D3D12CostCounter
{
public:
    explicit D3D12CostCounter(const char* counterLabel)
        : label(counterLabel)
    {
    }

    ~D3D12CostCounter()
    {
        if (!isReportingEnabled())
            return;

        std::fprintf(stderr,
                     "[%s] count=%d totalSeconds=%.3f meanMicros=%.1f "
                     "totalMB=%.1f\n",
                     label,
                     count,
                     seconds,
                     count > 0 ? seconds * 1e6 / (double) count : 0.0,
                     (double) bytes / (1024.0 * 1024.0));
    }

    void record(double elapsedSeconds, std::size_t byteCount)
    {
        ++count;
        seconds += elapsedSeconds;
        bytes += byteCount;
    }

private:
    static bool isReportingEnabled()
    {
        auto length = std::size_t {0};
        char value[8] = {};

        return getenv_s(&length, value, sizeof(value), "EACP_BUFFER_STATS") == 0
            && length > 1 && value[0] != '0';
    }

    const char* label;
    int count = 0;
    double seconds = 0.0;
    std::size_t bytes = 0;
};

// Times one call into it. Costs a steady_clock read whether or not the
// reporting is on, which is what keeps the counted paths identical.
class ScopedD3D12Cost
{
public:
    explicit ScopedD3D12Cost(D3D12CostCounter& counterToUse,
                             std::size_t byteCount = 0)
        : counter(counterToUse)
        , bytes(byteCount)
    {
    }

    ~ScopedD3D12Cost()
    {
        auto elapsed = std::chrono::steady_clock::now() - startedAt;
        counter.record(std::chrono::duration<double>(elapsed).count(), bytes);
    }

private:
    D3D12CostCounter& counter;
    std::size_t bytes;
    std::chrono::steady_clock::time_point startedAt =
        std::chrono::steady_clock::now();
};
}
