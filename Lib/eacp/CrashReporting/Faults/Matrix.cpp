#include "Matrix.h"

namespace eacp::CrashReporting::Faults
{
namespace
{
using namespace std::string_view_literals;

constexpr auto threadWatchPullRequest = 0;

struct LaneName
{
    Lane lane;
    std::string_view name;
};

constexpr auto laneNames = Array {
    LaneName {Lane::macOSArm64, "macOS arm64"},
    LaneName {Lane::macOSX64, "macOS x86_64"},
    LaneName {Lane::windowsX64Msvc, "Windows x64 MSVC"},
    LaneName {Lane::windowsX64ClangCl, "Windows x64 clang-cl"},
    LaneName {Lane::windowsArm64Msvc, "Windows ARM64 MSVC"},
    LaneName {Lane::windowsArm64ClangCl, "Windows ARM64 clang-cl"},
    LaneName {Lane::linux, "Linux"},
};

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

// macOS: the handler read it from the Mach exception, with a minidump.
ExpectedOutcome reportedThroughMach(Lane lane, std::string_view reason)
{
    auto outcome = reported(lane, reason);
    outcome.capturedBy = CaptureSource::handlerMach;
    outcome.hasMinidump = true;
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

ExpectedOutcome expectDivide(Lane lane)
{
    if (lane == Lane::macOSArm64)
        return notAvailable("arm64 does not trap an integer divide by zero");

    if (lane == Lane::windowsArm64ClangCl)
        return notAvailable("clang-cl emits no divide-by-zero check on ARM64");

    if (isMac(lane))
        return reportedThroughMach(lane, "EXC_ARITHMETIC");

    return reported(lane, "EXCEPTION_INT_DIVIDE_BY_ZERO");
}

ExpectedOutcome expectStackOverflow(Lane lane, ThreadChoice thread)
{
    if (!isMac(lane))
        return reported(lane, "EXCEPTION_STACK_OVERFLOW");

    if (!isBackground(thread))
        return reportedThroughMach(lane, "EXC_BAD_ACCESS");

    return withLimit(reportedThroughMach(lane, "EXC_BAD_ACCESS"),
                     "every frame is the recursion, so the thread's entry is "
                     "not reached");
}

ExpectedOutcome expectSoftwareCrash(Lane lane,
                                    ThreadChoice thread,
                                    std::string_view windowsReason)
{
    if (isMac(lane))
        return reportedThroughMach(lane, "SIGABRT");

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
        return reportedThroughMach(lane, "std::terminate");

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
    if (isArm64Windows(lane))
        return withShortStack(reported(lane, "STATUS_HEAP_CORRUPTION"),
                              "through the Windows Error Reporting helper; the "
                              "registers Windows hands it on the CI runners "
                              "have no frame chain, so the stack stops in ntdll");

    if (!isMac(lane))
        return reported(lane, "STATUS_HEAP_CORRUPTION");

    if (!isBackground(thread))
        return reportedThroughMach(lane, "EXC_BREAKPOINT");

    auto outcome = withShortStack(reportedThroughMach(lane, "SIGABRT"),
                                  "another thread's blocks go through the zone "
                                  "allocator, which aborts; some runs trap "
                                  "instead, in whichever thread allocates next");
    outcome.alternativeReason = "EXC_BREAKPOINT";
    return outcome;
}

ExpectedOutcome expectHeapSmash(Lane lane, ThreadChoice thread)
{
    if (!isMac(lane))
        return notAvailable("only macOS captures from outside the app, which "
                            "is what lets the report survive the heap");

    if (!isBackground(thread))
        return reportedThroughMach(lane, "EXC_BAD_ACCESS");

    auto outcome = withShortStack(reportedThroughMach(lane, "EXC_BAD_ACCESS"),
                                  "the main thread's own allocations can meet "
                                  "the wrecked heap first, and that thread is "
                                  "the one reported, trapping in malloc");
    outcome.alternativeReason = "EXC_BREAKPOINT";
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

ExpectedOutcome expectFastFail(Lane lane)
{
    if (isMac(lane))
        return notAvailable("__fastfail is Windows-only");

    return reported(lane, "STATUS_STACK_BUFFER_OVERRUN");
}

ExpectedOutcome expectIllegalInstruction(Lane lane)
{
    if (isMac(lane))
        return reportedThroughMach(lane, "EXC_BAD_INSTRUCTION");

    return reported(lane, "EXCEPTION_ILLEGAL_INSTRUCTION");
}

ExpectedOutcome expectPrivilegedInstruction(Lane lane)
{
    if (isMac(lane))
        return notAvailable("the fault halts from user mode on Windows x64 only");

    if (isArm64Windows(lane))
        return notAvailable("ARM64 has no privileged-instruction trap of its own: "
                            "such an instruction is an illegal one");

    return reported(lane, "EXCEPTION_PRIV_INSTRUCTION");
}

ExpectedOutcome expectFloatingPoint(Lane lane)
{
    if (lane == Lane::macOSArm64 || isArm64Windows(lane))
        return notAvailable("ARM64 cores do not implement floating-point traps");

    if (isMac(lane))
        return reportedThroughMach(lane, "EXC_ARITHMETIC");

    return reported(lane, "EXCEPTION_FLT_DIVIDE_BY_ZERO");
}

ExpectedOutcome expectExecuteProtection(Lane lane)
{
    if (isMac(lane))
        return reportedThroughMach(lane, "EXC_BAD_ACCESS");

    return reported(lane, "EXCEPTION_ACCESS_VIOLATION");
}

ExpectedOutcome expectWindowsOnly(Lane lane, std::string_view reason)
{
    if (isMac(lane))
        return notAvailable("a Windows memory-manager exception");

    return reported(lane, reason);
}

// Raised through RaiseException, which the ARM64 walk may not get past.
ExpectedOutcome expectInPageError(Lane lane)
{
    auto outcome = expectWindowsOnly(lane, "EXCEPTION_IN_PAGE_ERROR");

    if (isArm64Windows(lane))
        return withShortStack(outcome,
                              "StackWalk64 stops at RaiseException, one frame, "
                              "on the CI runners; the minidump has the thread");

    return outcome;
}

ExpectedOutcome expectMacOnly(Lane lane, std::string_view reason)
{
    if (!isMac(lane))
        return notAvailable("no such trap on Windows");

    return reportedThroughMach(lane, reason);
}

// The kernel raises a signal, which reaches the handler as EXC_CRASH.
ExpectedOutcome expectBadSystemCall(Lane lane)
{
    if (!isMac(lane))
        return notAvailable("no such signal on Windows");

    return reportedThroughMach(lane, "SIGSYS");
}

ExpectedOutcome expectOutcome(std::string_view fault, ThreadChoice thread, Lane lane)
{
    if (lane == Lane::linux)
        return notAvailable("no crash reporter on Linux");

    if (fault == "nullptr")
    {
        if (isMac(lane))
            return reportedThroughMach(lane, "EXC_BAD_ACCESS");

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

    if (fault == "heapsmash")
        return expectHeapSmash(lane, thread);

    if (fault == "hang" || fault == "spin" || fault == "deadlock")
        return expectStall(lane, thread);

    if (fault == "fastfail")
        return expectFastFail(lane);

    if (fault == "illegalinstruction")
        return expectIllegalInstruction(lane);

    if (fault == "privilegedinstruction")
        return expectPrivilegedInstruction(lane);

    if (fault == "floatingpoint")
        return expectFloatingPoint(lane);

    if (fault == "executenx")
        return expectExecuteProtection(lane);

    if (fault == "guardpage")
        return expectWindowsOnly(lane, "STATUS_GUARD_PAGE_VIOLATION");

    if (fault == "inpageerror")
        return expectInPageError(lane);

    if (fault == "buserror")
        return expectMacOnly(lane, "EXC_BAD_ACCESS");

    if (fault == "guardedfd")
        return expectMacOnly(lane, "EXC_GUARD");

    if (fault == "sigsys")
        return expectBadSystemCall(lane);

    return notAvailable("not in the matrix");
}

constexpr auto matrixFaults = Array {
    "nullptr"sv,
    "divide"sv,
    "stackoverflow"sv,
    "abort"sv,
    "purevirtual"sv,
    "terminate"sv,
    "heapcorruption"sv,
    "heapsmash"sv,
    "hang"sv,
    "spin"sv,
    "deadlock"sv,
    "fastfail"sv,
    "illegalinstruction"sv,
    "privilegedinstruction"sv,
    "floatingpoint"sv,
    "executenx"sv,
    "guardpage"sv,
    "inpageerror"sv,
    "buserror"sv,
    "guardedfd"sv,
    "sigsys"sv,
};

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

} // namespace eacp::CrashReporting::Faults
