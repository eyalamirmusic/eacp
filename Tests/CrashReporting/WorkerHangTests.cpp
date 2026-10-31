#include "CrashingProcess.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/CrashReporting/ReportDatabase.h>
#include <eacp/CrashReporting/SharedState.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <memory>

using namespace nano;
using namespace eacp::CrashReporting;
using namespace eacp::CrashReporting::Detail;

namespace WorkerHangTests
{
constexpr auto testThreshold = std::uint64_t {500};
constexpr auto testPollInterval = std::uint64_t {100};
constexpr auto testThreadId = std::uint64_t {42};
constexpr auto otherThreadId = std::uint64_t {43};
constexpr auto overdue = testThreshold + testPollInterval;

// Near the real steady clock, as the heartbeats a live thread sends are.
const auto heartbeatTime = getSteadyMilliseconds();

eacp::OwningPointer<SharedState> createWatchedState()
{
    auto state = eacp::makeOwned<SharedState>();
    initialiseSharedState(*state, 1);
    state->hang.thresholdMilliseconds = testThreshold;
    state->hang.pollIntervalMilliseconds = testPollInterval;
    return state;
}

SharedWatchedThread& claimAt(SharedHangInfo& hang,
                             std::string_view name,
                             std::uint64_t heartbeat,
                             std::uint64_t threadId = testThreadId)
{
    auto slot = claimWatchedThread(hang, name, threadId);
    check(slot.has_value());
    auto& thread = hang.threads[*slot];
    thread.lastHeartbeatMilliseconds = heartbeat;
    return thread;
}

auto tThreadSendingHeartbeatsIsNotReported =
    test("CrashReport/workerHangThreadSendingHeartbeatsIsNotReported") = []
{
    auto state = createWatchedState();
    claimAt(state->hang, "encoder", heartbeatTime);
    auto tracker = StallTracker {};

    check(takeNewStalls(state->hang, tracker, heartbeatTime + overdue - 1).empty());
};

auto tSilentThreadIsReportedOnce =
    test("CrashReport/workerHangSilentThreadIsReportedOncePerEpisode") = []
{
    auto state = createWatchedState();
    auto& thread = claimAt(state->hang, "encoder", heartbeatTime);
    auto tracker = StallTracker {};

    auto stalls = takeNewStalls(state->hang, tracker, heartbeatTime + overdue);
    check(stalls.size() == 1);
    check(stalls.size() == 1 && stalls[0].durationMilliseconds == overdue);
    check(takeNewStalls(state->hang, tracker, heartbeatTime + 2 * overdue).empty());

    auto nextHeartbeat = heartbeatTime + 3 * overdue;
    thread.lastHeartbeatMilliseconds = nextHeartbeat;
    check(takeNewStalls(state->hang, tracker, nextHeartbeat + overdue).size() == 1);
};

auto tPausedThreadIsNotReported =
    test("CrashReport/workerHangPausedThreadIsNotReported") = []
{
    auto state = createWatchedState();
    auto& thread = claimAt(state->hang, "encoder", heartbeatTime);
    auto tracker = StallTracker {};

    pauseWatchedThread(thread);
    check(takeNewStalls(state->hang, tracker, heartbeatTime + 10 * overdue).empty());

    resumeWatchedThread(thread);
    check(thread.pauseDepth.load() == 0);
    check(thread.lastHeartbeatMilliseconds.load() >= heartbeatTime);
};

auto tReleasedSlotIsFreed = test("CrashReport/workerHangReleasedSlotIsFreed") = []
{
    auto state = createWatchedState();
    auto& hang = state->hang;
    auto claimed = 0;

    while (claimWatchedThread(hang, "worker", testThreadId))
        ++claimed;

    check(claimed == static_cast<int>(maxWatchedThreads - 1));

    auto& released = hang.threads[mainThreadSlot + 1];
    releaseWatchedThread(released);
    released.lastHeartbeatMilliseconds = heartbeatTime;

    auto tracker = StallTracker {};
    auto stalls = takeNewStalls(hang, tracker, getSteadyMilliseconds() + overdue);
    check(std::ranges::none_of(stalls,
                               [](const NewStall& stall)
                               { return stall.slot == mainThreadSlot + 1; }));
    check(claimWatchedThread(hang, "again", testThreadId) == mainThreadSlot + 1);
};

auto tThreadsStalledTogetherAreOneReport =
    test("CrashReport/workerHangThreadsStalledTogetherAreOneReport") = []
{
    auto state = createWatchedState();
    claimAt(state->hang, "first", heartbeatTime);
    claimAt(
        state->hang, "second", heartbeatTime + testPollInterval / 4, otherThreadId);
    auto tracker = StallTracker {};

    auto stalls = takeNewStalls(state->hang, tracker, heartbeatTime + overdue);
    check(stalls.size() == 2);
};

auto tHealthyThreadIsNotTakenAlong =
    test("CrashReport/workerHangHealthyThreadIsNotTakenAlong") = []
{
    auto state = createWatchedState();
    claimAt(state->hang, "stuck", heartbeatTime);
    claimAt(
        state->hang, "due", heartbeatTime + overdue - testThreshold, otherThreadId);
    auto tracker = StallTracker {};

    check(takeNewStalls(state->hang, tracker, heartbeatTime + overdue).size() == 1);
};

auto tMainThreadWaitsForItsFirstHeartbeat =
    test("CrashReport/workerHangMainThreadWaitsForItsFirstHeartbeat") = []
{
    auto state = createWatchedState();
    watchMainThread(state->hang, testThreadId);
    auto tracker = StallTracker {};
    auto& main = state->hang.threads[mainThreadSlot];

    check(readBoundedText(main.name) == mainThreadName);
    check(takeNewStalls(state->hang, tracker, heartbeatTime).empty());

    main.lastHeartbeatMilliseconds = heartbeatTime;
    auto stalls = takeNewStalls(state->hang, tracker, heartbeatTime + overdue);
    check(stalls.size() == 1 && stalls[0].slot == mainThreadSlot);
};

auto tDetectionOffWatchesNothing =
    test("CrashReport/workerHangDetectionOffWatchesNothing") = []
{
    auto state = createWatchedState();
    state->hang.thresholdMilliseconds = 0;
    claimAt(state->hang, "encoder", heartbeatTime);
    auto tracker = StallTracker {};

    check(takeNewStalls(state->hang, tracker, heartbeatTime + 10 * overdue).empty());
};

auto tClaimedThreadWaitsForItsFirstHeartbeat =
    test("CrashReport/workerHangClaimedThreadWaitsForItsFirstHeartbeat") = []
{
    auto state = createWatchedState();
    check(claimWatchedThread(state->hang, "waiting", testThreadId).has_value());
    auto tracker = StallTracker {};

    check(takeNewStalls(state->hang, tracker, heartbeatTime + 10 * overdue).empty());
};

auto tSlotOfAnEndedThreadIsFreed =
    test("CrashReport/workerHangSlotOfAnEndedThreadIsFreed") = []
{
    auto state = createWatchedState();
    auto& hang = state->hang;
    auto ended = claimWatchedThread(hang, "ended", testThreadId);
    auto running = claimWatchedThread(hang, "running", otherThreadId);

    releaseWatchedThreadWithId(hang, testThreadId);

    check(hang.threads[*ended].status.load() == watchedThreadFree);
    check(hang.threads[*running].status.load() == watchedThreadLive);
};

constexpr auto workerHangThresholdMilliseconds = "500";
constexpr auto workerExitLimit = eacp::Time::MS {30000};
constexpr auto workerReportWaitLimit = eacp::Time::MS {10000};
constexpr auto workerPollInterval = eacp::Time::MS {20};

bool hasWorkerReport(const ReportDatabase& database)
{
    for (const auto& entry:
         eacp::Files::listDirectory(database.getPendingDirectory()))
    {
        if (eacp::Files::exists(entry.path / "report.json"))
            return true;
    }

    return false;
}

// The app quits on its own once the worker's stall is over; the handler may
// still be writing a report then.
eacp::Vector<CrashReport>
    runWorker(std::string_view behaviour,
              const eacp::Vector<std::string>& appArguments = {})
{
    auto database = eacp::TemporaryFile {};
    auto arguments = eacp::Vector<std::string> {"--worker",
                                                std::string {behaviour},
                                                "--hang-threshold-ms",
                                                workerHangThresholdMilliseconds,
                                                "--crash-database",
                                                database.path().str()};

    for (const auto& argument: appArguments)
        arguments.add(argument);

    auto process =
        eacp::Processes::Process {Tests::makeCrashingProcessOptions(arguments)};
    check(process.launched());

    auto exitDeadline = eacp::Time::Deadline {workerExitLimit};

    while (process.isRunning() && !exitDeadline.expired())
        eacp::Time::sleep(workerPollInterval);

    if (process.isRunning())
        process.kill();

    check(process.wait() == 0);

    auto reportDatabase = ReportDatabase {database.path()};
    auto reportDeadline = eacp::Time::Deadline {workerReportWaitLimit};
    auto hasStartedReport =
        !eacp::Files::listDirectory(reportDatabase.getPendingDirectory()).empty();

    while (hasStartedReport && !hasWorkerReport(reportDatabase)
           && !reportDeadline.expired())
        eacp::Time::sleep(workerPollInterval);

    return reportDatabase.readPendingReports();
}

void checkWorkerHangReport(const eacp::Vector<CrashReport>& reports)
{
    check(reports.size() == 1);

    if (reports.empty())
        return;

    const auto& report = reports[0];
    check(report.kind == ReportKind::hang);
    check(report.threadName == Tests::crashingProcessWorkerName);
    check(report.message.starts_with(Tests::crashingProcessWorkerName));
    check(report.stalledThreads.size() == 1);
}

auto tStalledWorkerIsReportedByName =
    test("CrashReporting/workerHangStalledWorkerIsReportedByName") = []
{ checkWorkerHangReport(runWorker("stall")); };

auto tWorkerWithNoHeartbeatIsNotReported =
    test("CrashReporting/workerHangWorkerWithNoHeartbeatIsNotReported") = []
{ check(runWorker("silent").empty()); };

auto tWorkerHangDetectionOffReportsNothing =
    test("CrashReporting/workerHangDetectionOffReportsNothing") = []
{ check(runWorker("stall", {"--without-worker-hang-detection"}).empty()); };

auto tEndedWorkersFreeTheirSlots =
    test("CrashReporting/workerHangEndedWorkersFreeTheirSlots") = []
{ checkWorkerHangReport(runWorker("stall-after-finished-workers")); };
} // namespace WorkerHangTests
