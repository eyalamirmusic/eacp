#include "CrashingProcess.h"
#include "ThreadStackProbe.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/App/App.h>
#include <eacp/Core/App/Args.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Plugins/DynamicLibrary.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/CrashReporting/CrashReporting.h>
#include <eacp/CrashReporting/Faults/Faults.h>

#include <algorithm>
#include <array>
#include <optional>
#include <thread>

namespace eacp::CrashReporting::Tests
{
namespace
{
using Faults::ThreadChoice;

constexpr auto hangBeyondThreshold = std::chrono::milliseconds {2000};
constexpr auto testEnvironment = "test";
constexpr auto testUserId = "crash-test-user";
constexpr auto testUserEmail = "user@example.com";
constexpr auto testUserName = "crashtest";
constexpr auto heartbeatsBeforeStalling = 3;

struct ReportingName
{
    std::string_view name;
    StackOverflowReporting reporting;
};

constexpr auto reportingNames = Array {
    ReportingName {"off", StackOverflowReporting::off},
    ReportingName {"startingThread", StackOverflowReporting::startingThread},
    ReportingName {"everyThread", StackOverflowReporting::everyThread},
};

std::optional<StackOverflowReporting> findReporting(std::string_view name)
{
    for (const auto& entry: reportingNames)
    {
        if (entry.name == name)
            return entry.reporting;
    }

    return std::nullopt;
}

struct CrashingProcessSettings
{
    std::string fault;
    std::string threadName = "main";
    FilePath databaseDirectory;
    std::int64_t hangThresholdMilliseconds = Options {}.hangThreshold.count();
    std::string stackOverflowReporting = "everyThread";
    bool pausesHangDetection = false;
    bool killsHandler = false;
    bool skipsCrashReporting = false;
    std::string probe;
    std::string libraryFunction;
};

FilePath writeSessionLog()
{
    auto log = FilePath::appCacheDirectory() / "CrashReportingTests.log";
    Files::removeFile(log);
    Files::appendFile(log, std::string_view {"crashing process started\n"});
    return log;
}

bool startCrashReporting(const CrashingProcessSettings& settings)
{
    auto reporting = findReporting(settings.stackOverflowReporting);

    if (!reporting)
        return false;

    auto options = Options {};
    options.hangThreshold =
        std::chrono::milliseconds {settings.hangThresholdMilliseconds};
    options.stackOverflowReporting = *reporting;
    options.environment = testEnvironment;
    options.attachments = {writeSessionLog()};

    if (!settings.databaseDirectory.empty())
        options.databaseDirectory = settings.databaseDirectory;

    start(options);
    setTag("project", crashingProcessProject);
    setExtra("launchedFrom", Files::executablePath().str());
    setUser({testUserId, testUserEmail, testUserName});
    addBreadcrumb({"app", "crash reporting started"});
    addBreadcrumb({"app", "about to crash", Level::warning});
    return true;
}

// Nothing is watched for a hang until the loop has beaten a few times.
Time::MS getDelayBeforeStalling()
{
    auto delay = Options {}.heartbeatInterval * heartbeatsBeforeStalling;
    return Time::MS {delay.count()};
}

// A main-thread stall quits once the fault returns; a background one leaves
// the main thread free, so the app runs as long as the stall would, then quits.
void runStallingFault(std::string_view name,
                      ThreadChoice thread,
                      bool pausesHangDetection)
{
    if (pausesHangDetection)
        pauseHangDetection();

    Faults::triggerFault(name, thread);

    if (pausesHangDetection)
        resumeHangDetection();

    if (thread == ThreadChoice::main)
    {
        // Quitting from inside the callback that stalled sometimes leaves
        // [NSApp run] waiting; from a fresh one it always ends.
        Threads::callAsync([] { Apps::quit(); });
        return;
    }

    auto stall = Faults::getHangDuration();
    Threads::callAfter(Time::MS {stall.count()}, [] { Apps::quit(); });
}

struct FaultApp
{
    FaultApp(const Faults::Fault& fault,
             ThreadChoice thread,
             bool pausesHangDetection)
    {
        if (!fault.stallsItsThread)
        {
            Faults::scheduleFault(fault.name, thread, Time::MS {0});
            return;
        }

        auto name = fault.name;
        Threads::callAfter(getDelayBeforeStalling(),
                           [name, thread, pausesHangDetection]
                           { runStallingFault(name, thread, pausesHangDetection); });
    }
};

int crashNow(const CrashingProcessSettings& settings)
{
    auto fault = Faults::findFault(settings.fault);
    auto thread = Faults::findThreadChoice(settings.threadName);

    if (!fault || !fault->get().isAvailable || !thread)
    {
        Console::printError("no fault called " + settings.fault + " on a "
                            + settings.threadName + " thread here");
        return Args::badArguments;
    }

    if (settings.killsHandler)
        Faults::killHandlerProcess();

    if (*thread == ThreadChoice::main && !fault->get().stallsItsThread)
    {
        Faults::triggerFault(settings.fault, ThreadChoice::main);
        return 0;
    }

    return Apps::run<FaultApp>(fault->get(), *thread, settings.pausesHangDetection);
}

// The library stays loaded for the rest of the process, which is short.
int crashInLoadedLibrary(const CrashingProcessSettings& settings)
{
    static auto library = Plugins::DynamicLibrary {FilePath {EACP_CRASHING_LIBRARY}};
    auto crash = library.findFunction<void (*)()>(settings.libraryFunction);
    auto thread = Faults::findThreadChoice(settings.threadName);

    if (crash == nullptr || !thread)
    {
        Console::printError("cannot call " + settings.libraryFunction
                            + " in the crashing library: " + library.lastError());
        return Args::badArguments;
    }

    if (*thread == ThreadChoice::main)
    {
        crash();
        return 0;
    }

    auto background = std::thread {crash};
    background.join();
    return 0;
}

std::optional<int> runThreadStackProbe(std::string_view probe)
{
    if (probe == "bench")
        return runThreadCreationBench();

    if (probe == "memory")
        return runThreadMemoryMeasurement();

    if (probe == "check")
        return runThreadStackCheck();

    if (probe == "overflow")
        return overflowANewStdThread();

    return std::nullopt;
}
} // namespace

bool isCrashingProcessCommandLine(int argc, char* argv[])
{
    return std::ranges::any_of(Span {argv, argc},
                               [](const char* argument)
                               { return argument == crashingProcessFlag; });
}

int runCrashingProcess()
{
    auto settings = CrashingProcessSettings {};
    auto isCrashingProcess = false;
    auto cli = Args::Parser {"CrashReportingTests --crashing-process",
                             "Starts the crash reporter and crashes, on purpose."};
    cli.flag(std::string {crashingProcessFlag}, isCrashingProcess, "this mode");
    cli.option("--crash", settings.fault, "the fault to raise");
    cli.option("--thread", settings.threadName, "main or background");
    cli.option("--crash-database", settings.databaseDirectory, "where reports go");
    cli.option("--hang-threshold-ms",
               settings.hangThresholdMilliseconds,
               "how long the main thread may stall; 0 turns hang detection off");
    cli.option("--stack-overflow-reporting",
               settings.stackOverflowReporting,
               "off, startingThread or everyThread");
    cli.flag("--pause-hang-detection",
             settings.pausesHangDetection,
             "pause hang detection around a hang or spin fault");
    cli.flag("--kill-handler",
             settings.killsHandler,
             "end the handler process first, so the crash goes unreported");
    cli.option("--thread-stack-probe",
               settings.probe,
               "bench (create and join std::threads), memory (resident KiB of "
               "100 live threads), check (which threads have a stack for "
               "overflow reports) or overflow (overflow a new std::thread)");
    cli.option("--crash-in-library",
               settings.libraryFunction,
               "the function to call in the crashing library, loaded at runtime");
    cli.flag("--without-crash-reporting",
             settings.skipsCrashReporting,
             "run the probe without start()");

    if (auto code = cli.exitCodeFor(cli.parse()))
        return *code;

    auto hangThreshold =
        std::chrono::milliseconds {settings.hangThresholdMilliseconds};
    Faults::setHangDuration(hangThreshold + hangBeyondThreshold);

    if (!settings.skipsCrashReporting && !startCrashReporting(settings))
        return Args::badArguments;

    if (auto code = runThreadStackProbe(settings.probe))
        return *code;

    if (!settings.libraryFunction.empty())
        return crashInLoadedLibrary(settings);

    return crashNow(settings);
}

Processes::ProcessOptions
    makeCrashingProcessOptions(const Vector<std::string>& arguments)
{
    auto options = Processes::ProcessOptions {};
    options.executable = Files::executablePath().str();
    options.arguments = {std::string {crashingProcessFlag}};

    for (const auto& argument: arguments)
        options.arguments.add(argument);

    return options;
}
} // namespace eacp::CrashReporting::Tests
