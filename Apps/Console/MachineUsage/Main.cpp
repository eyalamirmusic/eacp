#include <eacp/Core/App/Args.h>
#include <eacp/Core/Core.h>
#include <eacp/Core/Platform/MachineUsage.h>
#include <eacp/Core/Utils/Format.h>

#include <atomic>

using namespace eacp;

// Prints this process's and the machine's CPU and memory once a second, to be
// read beside top, Activity Monitor or Task Manager, for --seconds or until
// Ctrl-C.
namespace
{
constexpr auto bytesPerMebibyte = std::uint64_t {1024 * 1024};
constexpr auto percent = 100.0;
constexpr auto sampleInterval = Time::MS {1000};

std::atomic<bool> keepBurning {true};

std::string describeSystemFraction(const std::optional<double>& fraction)
{
    if (!fraction)
        return "unknown";

    return Strings::format("{:.1f}%", *fraction * percent);
}

void printReadings(Platform::CpuLoadMeter& meter)
{
    const auto load = meter.measureSinceLastCall();
    const auto memory = Platform::getMemoryUsage();

    Console::print(Strings::format(
        "process {:.2f} cores, {} MiB | machine {} busy, {} of {} MiB used",
        load.processCores,
        memory.processBytes / bytesPerMebibyte,
        describeSystemFraction(load.systemFraction),
        memory.systemUsedBytes / bytesPerMebibyte,
        memory.systemTotalBytes / bytesPerMebibyte));
}

void burnOneCore()
{
    while (keepBurning.load(std::memory_order_relaxed))
    {
    }
}

int printReadingsEverySecond()
{
    auto busyThreadCount = 0;
    auto secondsToRun = 0;

    auto cli =
        Args::Parser {"MachineUsage", "Prints CPU and memory use once a second."};
    cli.option("--busy-threads", busyThreadCount, "threads to keep spinning");
    cli.option("--seconds", secondsToRun, "seconds to run for; 0 runs until Ctrl-C");

    if (auto code = cli.exitCodeFor(cli.parse()))
        return *code;

    auto busyThreads = Vector<Threads::Thread> {};

    for (auto index = 0; index < busyThreadCount; ++index)
        busyThreads.create(burnOneCore);

    auto meter = Platform::CpuLoadMeter {};

    for (auto elapsedSeconds = 0; secondsToRun == 0 || elapsedSeconds < secondsToRun;
         ++elapsedSeconds)
    {
        Time::sleep(sampleInterval);
        printReadings(meter);
    }

    keepBurning = false;
    return 0;
}
} // namespace

int main(int argc, char* argv[])
{
    return Apps::runConsole(argc, argv, printReadingsEverySecond);
}
