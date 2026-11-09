#include "Common.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <optional>
#include <thread>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// Device::perDevice is kept by the Device rather than by its backend, so these
// run with or without a GPU: a Device that is not valid keeps its objects too.

namespace PerDeviceTests
{
namespace
{
struct Counted
{
    Counted()
        : number(++made)
    {
        ++alive;
    }

    ~Counted() { --alive; }

    static inline int made = 0;
    static inline int alive = 0;

    int number;
};

// Slow enough that every thread asking at once arrives while the first is still
// constructing it.
struct SlowToMake
{
    SlowToMake()
    {
        ++made;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    static inline std::atomic<int> made = 0;
};

struct Other
{
};

Device* deviceBeingAsked = nullptr;
bool innerAlive = false;
bool innerOutlivedOuter = false;

struct Inner
{
    Inner() { innerAlive = true; }
    ~Inner() { innerAlive = false; }
};

struct Outer
{
    Outer()
        : inner(deviceBeingAsked->perDevice<Inner>())
    {
    }

    ~Outer() { innerOutlivedOuter = innerAlive; }

    Inner& inner;
};
} // namespace

auto tPerDeviceIsOnePerDevice = test("GPU/perDeviceIsOnePerDevice") = []
{
    auto device = Device();
    const auto before = Counted::alive;

    auto& counted = device.perDevice<Counted>();

    check(&device.perDevice<Counted>() == &counted);
    check(Counted::alive == before + 1);
    check(static_cast<void*>(&device.perDevice<Other>())
          != static_cast<void*>(&counted));
};

auto tPerDeviceIsNotShared = test("GPU/perDeviceIsNotShared") = []
{
    auto first = Device();
    auto second = Device();

    check(&first.perDevice<Counted>() != &second.perDevice<Counted>());
};

auto tPerDeviceDiesWithItsDevice = test("GPU/perDeviceDiesWithItsDevice") = []
{
    const auto before = Counted::alive;

    {
        auto device = Device();
        device.perDevice<Counted>();
        check(Counted::alive == before + 1);
    }

    check(Counted::alive == before);
};

// What a static keyed by the Device's address gets wrong: a Device made in the
// storage of one destroyed before it has that one's address, and must not be
// handed that one's object.
auto tPerDeviceIsFreshForADeviceAtTheSameAddress =
    test("GPU/perDeviceIsFreshForADeviceAtTheSameAddress") = []
{
    const auto before = Counted::alive;
    auto device = std::optional<Device>();

    device.emplace();
    const auto* firstAddress = &*device;
    const auto firstNumber = device->perDevice<Counted>().number;

    device.reset();
    check(Counted::alive == before);

    device.emplace();
    check(&*device == firstAddress);

    check(device->perDevice<Counted>().number != firstNumber);
    check(Counted::alive == before + 1);
};

auto tPerDeviceIsMadeOnceAcrossThreads =
    test("GPU/perDeviceIsMadeOnceAcrossThreads") = []
{
    constexpr auto threadCount = 8;

    auto device = Device();
    SlowToMake::made = 0;

    auto found = Array<SlowToMake*, threadCount> {};
    auto start = std::latch(threadCount);
    auto threads = Vector<std::thread>();

    for (auto index = 0; index < threadCount; ++index)
    {
        threads.emplace_back(
            [&, index]
            {
                start.arrive_and_wait();
                found[index] = &device.perDevice<SlowToMake>();
            });
    }

    for (auto& thread: threads)
        thread.join();

    check(SlowToMake::made == 1);

    for (auto* object: found)
        check(object == &device.perDevice<SlowToMake>());
};

// The constructor asking for another of the same Device's objects neither
// deadlocks nor leaves it to be destroyed first.
auto tPerDeviceMadeInsideAnotherOutlivesIt =
    test("GPU/perDeviceMadeInsideAnotherOutlivesIt") = []
{
    innerOutlivedOuter = false;

    {
        auto device = Device();
        deviceBeingAsked = &device;

        check(&device.perDevice<Outer>().inner == &device.perDevice<Inner>());
        deviceBeingAsked = nullptr;
    }

    check(innerOutlivedOuter);
    check(!innerAlive);
};
} // namespace PerDeviceTests
