#include "CrashingProcess.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/CrashReporting/ReportDatabase.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;
using namespace eacp::CrashReporting;

namespace StackOverflowTests
{
constexpr auto overflowReportWaitLimit = Time::MS {10000};
constexpr auto unreportableOverflowWaitLimit = Time::MS {3000};
constexpr auto startingThreadHasStack = std::string_view {"starting thread: 1"};
constexpr auto startingThreadHasNoStack = std::string_view {"starting thread: 0"};
constexpr auto newThreadHasStack = std::string_view {"new thread: 1"};
constexpr auto newThreadHasNoStack = std::string_view {"new thread: 0"};

Processes::ProcessOptions makeThreadStackProbeOptions(std::string_view probe,
                                                      std::string_view reporting,
                                                      const FilePath& database)
{
    return Tests::makeCrashingProcessOptions({"--thread-stack-probe",
                                              std::string {probe},
                                              "--stack-overflow-reporting",
                                              std::string {reporting},
                                              "--crash-database",
                                              database.str()});
}

Processes::ProcessResult runStackOverflowProbe(std::string_view mode,
                                               std::string_view reporting,
                                               const FilePath& database)
{
    return Processes::run(makeThreadStackProbeOptions(mode, reporting, database));
}

std::string checkStackOverflowStacks(std::string_view reporting)
{
    auto database = TemporaryFile {};
    return runStackOverflowProbe("check", reporting, database.path()).output;
}

bool hasOverflowReport(const FilePath& database)
{
    auto reportDatabase = ReportDatabase {database};
    auto isWritten = [&]
    {
        for (const auto& entry:
             Files::listDirectory(reportDatabase.getPendingDirectory()))
        {
            if (Files::exists(entry.path / "report.json"))
                return true;
        }

        return false;
    };

    return Threads::runEventLoopUntil(isWritten, overflowReportWaitLimit);
}

bool doesOverflowOnNewThreadReport(std::string_view reporting)
{
    auto database = TemporaryFile {};
    runStackOverflowProbe("overflow", reporting, database.path());
    return hasOverflowReport(database.path());
}

bool contains(const std::string& text, std::string_view wanted)
{
    return text.find(wanted) != std::string::npos;
}

auto tOffInstallsNothing = test("StackOverflow/offInstallsNothing") = []
{
    auto output = checkStackOverflowStacks("off");
    check(contains(output, startingThreadHasNoStack));
    check(contains(output, newThreadHasNoStack));
};

auto tStartingThreadOnly = test("StackOverflow/startingThreadCoversOnlyItself") = []
{
    auto output = checkStackOverflowStacks("startingThread");
    check(contains(output, startingThreadHasStack));
    check(contains(output, newThreadHasNoStack));
};

auto tEveryThread = test("StackOverflow/everyThreadCoversARawStdThread") = []
{
    auto output = checkStackOverflowStacks("everyThread");
    check(contains(output, startingThreadHasStack));
    check(contains(output, newThreadHasStack));
};

auto tEveryThreadReports =
    test("StackOverflow/everyThreadReportsAnOverflowOnARawStdThread") = []
{ check(doesOverflowOnNewThreadReport("everyThread")); };

// With no stack left for the signal handler, macOS faults again on delivery
// and the thread spins there instead of dying, so the probe is killed.
bool doesUnreportableOverflowReport(std::string_view reporting)
{
    auto database = TemporaryFile {};
    auto probe = Processes::Process {
        makeThreadStackProbeOptions("overflow", reporting, database.path())};
    Threads::runEventLoopUntil([&] { return !probe.isRunning(); },
                               unreportableOverflowWaitLimit);
    probe.kill();
    probe.wait();
    return hasOverflowReport(database.path());
}

// Windows raises the overflow with a page of guard left, enough to reach the
// out-of-process handler with no reserve at all.
auto tStartingThreadMissesOverflow =
    test("StackOverflow/startingThreadMissesAnOverflowOnARawStdThreadOnMacOS") = []
{
    if (Platform::isApple())
        check(!doesUnreportableOverflowReport("startingThread"));
};
} // namespace StackOverflowTests
