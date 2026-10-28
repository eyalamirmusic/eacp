#include "Matrix.h"

#include <array>

namespace eacp::CrashReporting::Zoo
{
namespace
{
constexpr auto werModulePullRequest = 180;
constexpr auto machCapturePullRequest = 182;
constexpr auto threadWatchPullRequest = 0;

struct LaneName
{
    Lane lane;
    std::string_view name;
};

constexpr auto laneNames = std::to_array<LaneName>({
    {Lane::macOSArm64, "macOS arm64"},
    {Lane::macOSX64, "macOS x86_64"},
    {Lane::windowsX64Msvc, "Windows x64 MSVC"},
    {Lane::windowsX64ClangCl, "Windows x64 clang-cl"},
    {Lane::windowsArm64Msvc, "Windows ARM64 MSVC"},
    {Lane::windowsArm64ClangCl, "Windows ARM64 clang-cl"},
    {Lane::linux, "Linux"},
});

bool isMac(Lane lane)
{
    return lane == Lane::macOSArm64 || lane == Lane::macOSX64;
}

bool isArm64Windows(Lane lane)
{
    return lane == Lane::windowsArm64Msvc || lane == Lane::windowsArm64ClangCl;
}

bool isClangCl(Lane lane)
{
    return lane == Lane::windowsX64ClangCl || lane == Lane::windowsArm64ClangCl;
}

bool isBackground(ThreadChoice thread)
{
    return thread == ThreadChoice::background;
}

// A crash the handler caught with the stack reaching the fault; Windows adds
// a minidump.
ExpectedOutcome reported(Lane lane, std::string_view reason)
{
    auto outcome = ExpectedOutcome {};
    outcome.status = OutcomeStatus::reported;
    outcome.reason = reason;
    outcome.hasStack = true;
    outcome.hasMinidump = !isMac(lane);
    outcome.stackReachesFault = true;
    return outcome;
}

ExpectedOutcome withLimit(ExpectedOutcome outcome, std::string_view note)
{
    outcome.status = OutcomeStatus::reportedWithLimits;
    outcome.note = note;
    return outcome;
}

ExpectedOutcome withShortStack(ExpectedOutcome outcome, std::string_view note)
{
    outcome.stackReachesFault = false;
    return withLimit(outcome, note);
}

ExpectedOutcome notAvailable(std::string_view note)
{
    auto outcome = ExpectedOutcome {};
    outcome.status = OutcomeStatus::notAvailable;
    outcome.note = note;
    return outcome;
}

ExpectedOutcome pending(std::string_view note, int pullRequest)
{
    auto outcome = ExpectedOutcome {};
    outcome.status = OutcomeStatus::pendingPR;
    outcome.note = note;
    outcome.pullRequest = pullRequest;
    return outcome;
}

// Windows sees these only in the app's exit code, after the app is gone.
ExpectedOutcome reportedFromExitCode(std::string_view reason, ThreadChoice thread)
{
    auto outcome = ExpectedOutcome {};
    outcome.status = OutcomeStatus::reportedWithLimits;
    outcome.reason = reason;
    outcome.capturedBy = CaptureSource::exitCode;
    outcome.pullRequest = werModulePullRequest;
    outcome.note = "from the exit code: no stack, no minidump";

    if (isBackground(thread))
        outcome.note = "from the exit code: no stack, no minidump, and the "
                       "report names no crashing thread";

    return outcome;
}

ExpectedOutcome expectDivide(Lane lane)
{
    if (lane == Lane::macOSArm64)
        return notAvailable("arm64 does not trap an integer divide by zero");

    if (lane == Lane::windowsArm64ClangCl)
        return notAvailable("clang-cl emits no divide-by-zero check on ARM64");

    if (isMac(lane))
        return reported(lane, "SIGFPE");

    return reported(lane, "EXCEPTION_INT_DIVIDE_BY_ZERO");
}

ExpectedOutcome expectStackOverflow(Lane lane, ThreadChoice thread)
{
    if (!isMac(lane))
        return reported(lane, "EXCEPTION_STACK_OVERFLOW");

    if (!isBackground(thread))
        return reported(lane, "SIGSEGV");

    return withLimit(reported(lane, "SIGBUS"),
                     "a secondary thread's guard page is a protection fault, "
                     "so SIGBUS; every frame is the recursion");
}

ExpectedOutcome expectSoftwareCrash(Lane lane,
                                    ThreadChoice thread,
                                    std::string_view windowsReason)
{
    if (isMac(lane))
        return reported(lane, "SIGABRT");

    auto outcome = reported(lane, windowsReason);

    if (isArm64Windows(lane) && isBackground(thread))
        return withShortStack(outcome,
                              "StackWalk64 stops inside the CRT, short of the "
                              "faulting code");

    return outcome;
}

ExpectedOutcome expectTerminate(Lane lane, ThreadChoice thread)
{
    if (isMac(lane))
        return reported(lane, "std::terminate");

    if (isClangCl(lane) && isBackground(thread))
    {
        auto outcome = withLimit(reported(lane, "SIGABRT"),
                                 "clang-cl's std::thread terminates with no "
                                 "exception current: no type or what()");
        outcome.stackReachesFault = !isArm64Windows(lane);
        return outcome;
    }

    if (isArm64Windows(lane))
        return withShortStack(reported(lane, "std::terminate"),
                              "StackWalk64 stops at RaiseException, one frame, "
                              "on the CI runners; the minidump has the thread");

    return reported(lane, "std::terminate");
}

ExpectedOutcome expectHeapCorruption(Lane lane, ThreadChoice thread)
{
    if (!isMac(lane))
        return reportedFromExitCode("STATUS_HEAP_CORRUPTION", thread);

    if (!isBackground(thread))
        return reported(lane, "SIGTRAP");

    auto outcome = withLimit(reported(lane, "SIGABRT"),
                             "another thread's blocks go through the zone "
                             "allocator, which aborts; some runs trap instead");
    outcome.alternativeReason = "SIGTRAP";
    return outcome;
}

ExpectedOutcome expectStall(Lane lane, ThreadChoice thread)
{
    if (isBackground(thread))
        return pending("only the main thread is watched for hangs",
                       threadWatchPullRequest);

    auto outcome = reported(lane, hangReason);

    if (isArm64Windows(lane))
        return withShortStack(outcome,
                              "the frame-pointer walk can skip a stalled leaf "
                              "built without a frame record");

    return outcome;
}

ExpectedOutcome expectFastFail(Lane lane, ThreadChoice thread)
{
    if (isMac(lane))
        return notAvailable("__fastfail is Windows-only");

    return reportedFromExitCode("STATUS_STACK_BUFFER_OVERRUN", thread);
}

ExpectedOutcome expectOutcome(std::string_view fault, ThreadChoice thread, Lane lane)
{
    if (lane == Lane::linux)
        return notAvailable("no crash reporter on Linux");

    if (fault == "nullptr")
    {
        if (isMac(lane))
            return reported(lane, "SIGSEGV");

        return reported(lane, "EXCEPTION_ACCESS_VIOLATION");
    }

    if (fault == "divide")
        return expectDivide(lane);

    if (fault == "stackoverflow")
        return expectStackOverflow(lane, thread);

    if (fault == "abort")
        return expectSoftwareCrash(lane, thread, "SIGABRT");

    if (fault == "purevirtual")
        return expectSoftwareCrash(lane, thread, "purecall");

    if (fault == "terminate")
        return expectTerminate(lane, thread);

    if (fault == "heapcorruption")
        return expectHeapCorruption(lane, thread);

    if (fault == "hang" || fault == "spin" || fault == "deadlock")
        return expectStall(lane, thread);

    if (fault == "fastfail")
        return expectFastFail(lane, thread);

    return notAvailable("not in the matrix");
}

constexpr auto matrixFaults = std::to_array<std::string_view>({
    "nullptr",
    "divide",
    "stackoverflow",
    "abort",
    "purevirtual",
    "terminate",
    "heapcorruption",
    "hang",
    "spin",
    "deadlock",
    "fastfail",
});

Vector<MatrixCell> makeCrashMatrix()
{
    auto cells = Vector<MatrixCell> {};

    for (auto lane: listLanes())
    {
        for (auto fault: matrixFaults)
        {
            for (auto thread: {ThreadChoice::main, ThreadChoice::background})
                cells.add({fault, thread, lane, expectOutcome(fault, thread, lane)});
        }
    }

    return cells;
}

Vector<LaneGap> makeLaneGaps()
{
    auto gaps = Vector<LaneGap> {};

    for (auto lane: {Lane::macOSArm64, Lane::macOSX64})
    {
        gaps.add({lane,
                  "capture is in-process with backtrace(): no minidump, and "
                  "no stacks of other threads",
                  machCapturePullRequest});
    }

    for (auto lane: {Lane::windowsX64Msvc,
                     Lane::windowsX64ClangCl,
                     Lane::windowsArm64Msvc,
                     Lane::windowsArm64ClangCl})
    {
        gaps.add({lane,
                  "with the handler gone, faults seen only in the exit code "
                  "(heap corruption, __fastfail) are lost",
                  werModulePullRequest});
    }

    gaps.add({Lane::macOSX64,
              "no machine runs this lane: its cells are expected, not verified",
              0});
    gaps.add({Lane::linux, "no crash reporter on Linux", 0});
    return gaps;
}

std::string_view getStatusSymbol(OutcomeStatus status)
{
    switch (status)
    {
        case OutcomeStatus::reported:
            return "✅";
        case OutcomeStatus::reportedWithLimits:
            return "⚠️";
        case OutcomeStatus::notReported:
            return "❌";
        case OutcomeStatus::notAvailable:
            return "—";
        case OutcomeStatus::pendingPR:
            return "⏳";
    }

    return {};
}

std::string describePullRequest(int pullRequest)
{
    if (pullRequest == 0)
        return "PR not opened yet";

    return "PR #" + std::to_string(pullRequest);
}

std::string describeCapture(const ExpectedOutcome& outcome)
{
    auto text = std::string {getCaptureSourceName(outcome.capturedBy)};

    if (outcome.hasStack)
        text += ", stack";
    else
        text += ", no stack";

    if (outcome.hasMinidump)
        text += ", minidump";

    return text;
}

std::string renderCell(const ExpectedOutcome& outcome)
{
    auto symbol = std::string {getStatusSymbol(outcome.status)};

    switch (outcome.status)
    {
        case OutcomeStatus::reported:
            return symbol + " " + std::string {outcome.reason} + " ("
                   + describeCapture(outcome) + ")";
        case OutcomeStatus::reportedWithLimits:
            return symbol + " " + std::string {outcome.reason} + " ("
                   + describeCapture(outcome) + ")";
        case OutcomeStatus::notReported:
            return symbol + " " + std::string {outcome.note};
        case OutcomeStatus::notAvailable:
            return symbol;
        case OutcomeStatus::pendingPR:
            return symbol + " " + describePullRequest(outcome.pullRequest);
    }

    return {};
}

std::string renderLaneTable(Lane lane)
{
    auto text = "## " + std::string {getLaneName(lane)} + "\n\n"
                + "| Fault | Main thread | Background thread |\n"
                + "|---|---|---|\n";

    for (auto fault: matrixFaults)
    {
        auto label = std::string {fault};

        if (auto found = findFault(fault))
            label = std::string {found->get().label};

        text +=
            "| " + label + " | "
            + renderCell(*findExpectedOutcome(fault, ThreadChoice::main, lane))
            + " | "
            + renderCell(*findExpectedOutcome(fault, ThreadChoice::background, lane))
            + " |\n";
    }

    return text + "\n";
}

bool isGap(const ExpectedOutcome& outcome)
{
    return outcome.status == OutcomeStatus::reportedWithLimits
           || outcome.status == OutcomeStatus::notReported
           || outcome.status == OutcomeStatus::pendingPR;
}

std::string renderGap(const MatrixCell& cell)
{
    const auto& outcome = cell.outcome;
    auto text = "- " + std::string {getStatusSymbol(outcome.status)} + " "
                + std::string {getLaneName(cell.lane)} + ", "
                + std::string {cell.fault} + " on the "
                + std::string {getThreadChoiceName(cell.thread)}
                + " thread: " + std::string {outcome.note};

    if (outcome.pullRequest != 0 || outcome.status == OutcomeStatus::pendingPR)
        text += " (" + describePullRequest(outcome.pullRequest) + ")";

    return text + "\n";
}

std::string renderGaps()
{
    auto text = std::string {"## Gaps\n\n"};

    for (const auto& gap: getLaneGaps())
    {
        text += "- " + std::string {getLaneName(gap.lane)} + ": "
                + std::string {gap.note};

        if (gap.pullRequest != 0)
            text += " (" + describePullRequest(gap.pullRequest) + ")";

        text += "\n";
    }

    for (const auto& cell: getCrashMatrix())
    {
        if (isGap(cell.outcome))
            text += renderGap(cell);
    }

    return text;
}
} // namespace

const Vector<Lane>& listLanes()
{
    static const auto lanes = []
    {
        auto all = Vector<Lane> {};

        for (const auto& entry: laneNames)
            all.add(entry.lane);

        return all;
    }();

    return lanes;
}

std::string_view getLaneName(Lane lane)
{
    for (const auto& entry: laneNames)
        if (entry.lane == lane)
            return entry.name;

    return {};
}

Lane getCurrentLane()
{
#if defined(__APPLE__) && defined(__aarch64__)
    return Lane::macOSArm64;
#elif defined(__APPLE__)
    return Lane::macOSX64;
#elif defined(_M_ARM64) && defined(__clang__)
    return Lane::windowsArm64ClangCl;
#elif defined(_M_ARM64)
    return Lane::windowsArm64Msvc;
#elif defined(_WIN32) && defined(__clang__)
    return Lane::windowsX64ClangCl;
#elif defined(_WIN32)
    return Lane::windowsX64Msvc;
#else
    return Lane::linux;
#endif
}

const Vector<MatrixCell>& getCrashMatrix()
{
    static const auto cells = makeCrashMatrix();
    return cells;
}

const Vector<LaneGap>& getLaneGaps()
{
    static const auto gaps = makeLaneGaps();
    return gaps;
}

std::optional<ExpectedOutcome>
    findExpectedOutcome(std::string_view fault, ThreadChoice thread, Lane lane)
{
    for (const auto& cell: getCrashMatrix())
    {
        if (cell.fault == fault && cell.thread == thread && cell.lane == lane)
            return cell.outcome;
    }

    return std::nullopt;
}

std::string renderCrashMatrix()
{
    auto text = std::string {
        "# Crash matrix\n\n"
        "Generated from `Zoo/Matrix.cpp` by `SelfDestruct --matrix <file>`; "
        "`CrashMatrix/fileIsCurrent` fails when this file differs. Every "
        "reported cell on the lane a test binary is built for is run by "
        "`Tests/CrashReporting`.\n\n"
        "Legend: ✅ reported (who captured it, stack, minidump) · ⚠️ reported "
        "with a limit, listed under Gaps · ❌ not reported · ⏳ pending a PR · "
        "— not available on that lane.\n\n"};

    for (auto lane: listLanes())
        text += renderLaneTable(lane);

    return text + renderGaps();
}
} // namespace eacp::CrashReporting::Zoo
