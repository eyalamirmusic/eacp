#include "Common.h"

#include <eacp/Core/Platform/MachineUsage.h>
#include <eacp/Core/Platform/MachineUsagePlatform.h>
#include <eacp/Core/Platform/Platform.h>

#include <algorithm>
#include <chrono>

using namespace nano;

namespace MachineUsageTests
{
using eacp::LOG;
using eacp::Platform::CpuLoadMeter;
using eacp::Platform::Detail::advanceWrappingCounter;
using eacp::Platform::Detail::calculateCpuLoad;
using eacp::Platform::Detail::CpuTimes;
using eacp::Platform::Detail::readProcessCpuNanoseconds;
using eacp::Platform::Detail::SystemCpuTicks;

constexpr auto burnDuration = eacp::Time::MS {500};
constexpr auto megabyte = std::size_t {1024 * 1024};

bool isFraction(double value)
{
    return value >= 0.0 && value <= 1.0;
}

void burnOneCoreFor(eacp::Time::MS duration)
{
    const auto deadline = eacp::Time::Deadline {duration};

    while (!deadline.expired())
    {
    }
}

constexpr auto burnNanoseconds = std::int64_t {500'000'000};

void burnProcessCpuTime()
{
    constexpr auto giveUpAfter = eacp::Time::MS {20'000};

    const auto cpuStart = readProcessCpuNanoseconds();
    const auto deadline = eacp::Time::Deadline {giveUpAfter};

    while (readProcessCpuNanoseconds() - cpuStart < burnNanoseconds
           && !deadline.expired())
    {
    }
}

auto tWrappingCounterKeepsCounting =
    test("MachineUsage/aWrappedTickCounterStillCountsForward") = []
{
    constexpr auto widenedSoFar = std::uint64_t {1'000'000};
    constexpr auto beforeWrap = std::uint32_t {0xFFFF'FFF0};
    constexpr auto afterWrap = std::uint32_t {0x10};
    constexpr auto ticksAcrossTheWrap = std::uint64_t {0x20};

    check(advanceWrappingCounter(widenedSoFar, beforeWrap, afterWrap)
          == widenedSoFar + ticksAcrossTheWrap);
    check(advanceWrappingCounter(widenedSoFar, afterWrap, afterWrap)
          == widenedSoFar);
};

auto tCalculatesCoresAndFraction =
    test("MachineUsage/loadIsCpuTimeOverWallTimeAndBusyOverTotal") = []
{
    constexpr auto second = std::int64_t {1'000'000'000};

    const auto earlier = CpuTimes {.system = SystemCpuTicks {}};
    const auto later = CpuTimes {.wallNanoseconds = 2 * second,
                                 .processCpuNanoseconds = 5 * second,
                                 .system = SystemCpuTicks {.busy = 3, .total = 4}};

    const auto load = calculateCpuLoad(earlier, later);

    check(load.processCores == 2.5);
    check(load.systemFraction == 0.75);
};

auto tNoTimeIsNoLoad = test("MachineUsage/noElapsedTimeReadsAsNoLoad") = []
{
    const auto reading = CpuTimes {.wallNanoseconds = 7,
                                   .processCpuNanoseconds = 7,
                                   .system = SystemCpuTicks {.busy = 1, .total = 2}};

    const auto load = calculateCpuLoad(reading, reading);

    check(load.processCores == 0.0);
    check(load.systemFraction == 0.0);
};

auto tMissingSystemIsEmpty =
    test("MachineUsage/aMissingSystemReadingLeavesTheFractionEmpty") = []
{
    const auto withSystem = CpuTimes {.system = SystemCpuTicks {}};
    const auto withoutSystem = CpuTimes {.wallNanoseconds = 1};

    check(!calculateCpuLoad(withSystem, withoutSystem).systemFraction);
    check(!calculateCpuLoad(withoutSystem, withSystem).systemFraction);
};

// Load-proof: the thread burns a fixed amount of CPU time however long the
// machine takes to give it, and the bound is that time over the wall time seen.
auto tBusyThreadIsCounted =
    test("MachineUsage/aBusyThreadReadsAsItsCpuTimeOverTheWallTime") = []
{
    constexpr auto mostCoresOneThreadCanRead = 1.5;

    const auto wallStart = std::chrono::steady_clock::now();
    auto meter = CpuLoadMeter {};

    auto burner = eacp::Threads::Thread {burnProcessCpuTime};
    burner.join();

    const auto load = meter.measureSinceLastCall();
    const auto wall = std::chrono::steady_clock::now() - wallStart;
    const auto wallNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count();
    const auto fewestCores =
        static_cast<double>(burnNanoseconds) / static_cast<double>(wallNanoseconds);
    LOG("one busy thread read as ",
        load.processCores,
        " cores, at least ",
        fewestCores);

    check(load.processCores >= fewestCores);
    check(load.processCores < mostCoresOneThreadCanRead);
};

auto tSystemFractionIsAFraction =
    test("MachineUsage/theSystemFractionIsBetweenZeroAndOne") = []
{
    auto meter = CpuLoadMeter {};
    burnOneCoreFor(burnDuration);

    const auto load = meter.measureSinceLastCall();

    if constexpr (!eacp::Platform::isAndroid())
        check(load.systemFraction.has_value());

    if (load.systemFraction)
        check(isFraction(*load.systemFraction));
};

auto tFirstCallIsSane =
    test("MachineUsage/theFirstCallMeasuresFromConstruction") = []
{
    auto meter = CpuLoadMeter {};
    const auto load = meter.measureSinceLastCall();

    check(load.processCores >= 0.0);
};

auto tSystemMemoryIsSane = test("MachineUsage/systemMemoryIsUsedWithinTotal") = []
{
    const auto usage = eacp::Platform::getMemoryUsage();

    check(usage.systemTotalBytes > 0);
    check(usage.systemUsedBytes > 0);
    check(usage.systemUsedBytes <= usage.systemTotalBytes);
    check(usage.processBytes > 0);
    check(usage.processBytes < usage.systemTotalBytes);
};

// GCC removes a heap block nothing outside the test can see, along with the
// writes that fill it; storing its address in a volatile pointer keeps both.
std::uint8_t* volatile escapedBlock = nullptr;

void makeBlockVisibleOutsideTheTest(std::vector<std::uint8_t>& block)
{
    escapedBlock = block.data();
}

auto tTouchedBlockIsCounted =
    test("MachineUsage/touchingABlockRaisesTheProcessBytesByItsSize") = []
{
    constexpr auto blockBytes = 128 * megabyte;
    constexpr auto smallestCountedShare = 0.75;
    constexpr auto filler = std::uint8_t {1};

    const auto before = eacp::Platform::getMemoryUsage().processBytes;

    auto block = std::vector<std::uint8_t>(blockBytes);
    std::fill(block.begin(), block.end(), filler);
    makeBlockVisibleOutsideTheTest(block);

    const auto after = eacp::Platform::getMemoryUsage().processBytes;
    const auto rise = static_cast<double>(after) - static_cast<double>(before);
    LOG("touching ",
        blockBytes / megabyte,
        " MiB raised the process by ",
        rise / megabyte,
        " MiB");

    check(rise > smallestCountedShare * blockBytes);
    check(block.back() == filler);
};
} // namespace MachineUsageTests
