#include <eacp/CrashReporting/SharedState.h>
#include <eacp/CrashReporting/ThreadWatch.h>

#include <NanoTest/NanoTest.h>

#include <memory>

using namespace nano;
using namespace eacp::CrashReporting;
using namespace eacp::CrashReporting::Detail;

namespace ThreadWatchTests
{
constexpr auto testThreshold = std::uint64_t {500};
constexpr auto testPollInterval = std::uint64_t {100};
constexpr auto testThreadId = std::uint64_t {42};
constexpr auto otherThreadId = std::uint64_t {43};
constexpr auto beatTime = std::uint64_t {1000000};
constexpr auto overdue = testThreshold + testPollInterval;

std::unique_ptr<SharedState> createWatchedState()
{
    auto state = std::make_unique<SharedState>();
    initialiseSharedState(*state, 1);
    state->hang.thresholdMilliseconds = testThreshold;
    state->hang.pollIntervalMilliseconds = testPollInterval;
    return state;
}

SharedWatchedThread& claimAt(SharedHangInfo& hang,
                             std::string_view name,
                             std::uint64_t beat,
                             std::uint64_t threadId = testThreadId)
{
    auto slot = claimWatchedThread(hang, name, 0, threadId);
    check(slot.has_value());
    auto& thread = hang.threads[*slot];
    thread.lastBeatMilliseconds = beat;
    return thread;
}

auto tBeatingThreadIsNotReported =
    test("CrashReport/threadWatchBeatingThreadIsNotReported") = []
{
    auto state = createWatchedState();
    claimAt(state->hang, "encoder", beatTime);
    auto tracker = StallTracker {};

    check(takeNewStalls(state->hang, tracker, beatTime + overdue - 1).empty());
};

auto tSilentThreadIsReportedOnce =
    test("CrashReport/threadWatchSilentThreadIsReportedOncePerEpisode") = []
{
    auto state = createWatchedState();
    auto& thread = claimAt(state->hang, "encoder", beatTime);
    auto tracker = StallTracker {};

    auto stalls = takeNewStalls(state->hang, tracker, beatTime + overdue);
    check(stalls.size() == 1);
    check(stalls.size() == 1 && stalls[0].durationMilliseconds == overdue);
    check(takeNewStalls(state->hang, tracker, beatTime + 2 * overdue).empty());

    auto nextBeat = beatTime + 3 * overdue;
    thread.lastBeatMilliseconds = nextBeat;
    check(takeNewStalls(state->hang, tracker, nextBeat + overdue).size() == 1);
};

auto tPausedThreadIsNotReported =
    test("CrashReport/threadWatchPausedThreadIsNotReported") = []
{
    auto state = createWatchedState();
    auto& thread = claimAt(state->hang, "encoder", beatTime);
    auto tracker = StallTracker {};

    pauseWatchedThread(thread);
    check(takeNewStalls(state->hang, tracker, beatTime + 10 * overdue).empty());

    resumeWatchedThread(thread);
    check(thread.pauseDepth.load() == 0);
    check(thread.lastBeatMilliseconds.load() > beatTime);
};

auto tReleasedSlotIsFreed = test("CrashReport/threadWatchReleasedSlotIsFreed") = []
{
    auto state = createWatchedState();
    auto& hang = state->hang;
    auto claimed = 0;

    while (claimWatchedThread(hang, "worker", 0, testThreadId))
        ++claimed;

    check(claimed == static_cast<int>(maxWatchedThreads - 1));

    auto& released = hang.threads[mainThreadSlot + 1];
    releaseWatchedThread(released);
    released.lastBeatMilliseconds = beatTime;

    auto tracker = StallTracker {};
    check(takeNewStalls(hang, tracker, beatTime + overdue).empty());
    check(claimWatchedThread(hang, "again", 0, testThreadId) == mainThreadSlot + 1);
};

auto tThreadsStalledTogetherAreOneReport =
    test("CrashReport/threadWatchThreadsStalledTogetherAreOneReport") = []
{
    auto state = createWatchedState();
    claimAt(state->hang, "first", beatTime);
    claimAt(state->hang, "second", beatTime + testPollInterval / 4, otherThreadId);
    auto tracker = StallTracker {};

    auto stalls = takeNewStalls(state->hang, tracker, beatTime + overdue);
    check(stalls.size() == 2);
};

auto tHealthyThreadIsNotTakenAlong =
    test("CrashReport/threadWatchHealthyThreadIsNotTakenAlong") = []
{
    auto state = createWatchedState();
    claimAt(state->hang, "stuck", beatTime);
    claimAt(state->hang, "due", beatTime + overdue - testThreshold, otherThreadId);
    auto tracker = StallTracker {};

    check(takeNewStalls(state->hang, tracker, beatTime + overdue).size() == 1);
};

auto tMainThreadWaitsForItsFirstBeat =
    test("CrashReport/threadWatchMainThreadWaitsForItsFirstBeat") = []
{
    auto state = createWatchedState();
    watchMainThread(state->hang, testThreadId);
    auto tracker = StallTracker {};
    auto& main = state->hang.threads[mainThreadSlot];

    check(readBoundedText(main.name) == mainThreadName);
    check(takeNewStalls(state->hang, tracker, beatTime).empty());

    main.lastBeatMilliseconds = beatTime;
    auto stalls = takeNewStalls(state->hang, tracker, beatTime + overdue);
    check(stalls.size() == 1 && stalls[0].slot == mainThreadSlot);
};

auto tOwnThresholdIsUsed = test("CrashReport/threadWatchUsesItsOwnThreshold") = []
{
    constexpr auto longThreshold = std::uint64_t {5000};
    auto state = createWatchedState();
    auto slot = claimWatchedThread(state->hang, "slow", longThreshold, testThreadId);
    check(slot.has_value());
    state->hang.threads[*slot].lastBeatMilliseconds = beatTime;
    auto tracker = StallTracker {};

    check(takeNewStalls(state->hang, tracker, beatTime + overdue).empty());
    check(takeNewStalls(
              state->hang, tracker, beatTime + longThreshold + testPollInterval)
              .size()
          == 1);
};

auto tDetectionOffWatchesNothing =
    test("CrashReport/threadWatchDetectionOffWatchesNothing") = []
{
    auto state = createWatchedState();
    state->hang.thresholdMilliseconds = 0;
    claimAt(state->hang, "encoder", beatTime);
    auto tracker = StallTracker {};

    check(takeNewStalls(state->hang, tracker, beatTime + 10 * overdue).empty());
};

auto tWatchBeforeStartWatchesNothing =
    test("CrashReport/threadWatchBeforeStartWatchesNothing") = []
{
    auto watch = ThreadWatch {"unstarted"};
    check(!watch.isWatched());
    watch.beat();
    watch.pause();
    watch.resume();
};
} // namespace ThreadWatchTests
