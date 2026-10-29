#include "CrashingProcess.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/CrashReporting/Faults/Matrix.h>
#include <eacp/CrashReporting/ReportDatabase.h>

#include <NanoTest/NanoTest.h>

#include <algorithm>

using namespace nano;
using namespace eacp;
using namespace eacp::CrashReporting;

namespace LoadedLibraryTests
{
constexpr auto libraryReportWaitLimit = Time::MS {10000};
constexpr auto libraryFaultFunction =
    std::string_view {"crashingLibraryWriteThroughNullPointer"};
constexpr auto libraryThrowFunction =
    std::string_view {"crashingLibraryThrowUncaughtException"};
constexpr auto libraryThrownMessage =
    std::string_view {"the crashing library threw this on purpose"};

std::string getLibraryFileName()
{
    return FilePath {EACP_CRASHING_LIBRARY}.fileName();
}

std::optional<CrashReport> crashInLoadedLibrary(std::string_view function,
                                                std::string_view thread)
{
    auto database = TemporaryFile {};
    auto options = Tests::makeCrashingProcessOptions({"--crash-in-library",
                                                      std::string {function},
                                                      "--thread",
                                                      std::string {thread},
                                                      "--crash-database",
                                                      database.path().str()});
    auto result = Processes::run(options);
    check(result.launched);
    check(result.exitCode != 0);

    auto reportDatabase = ReportDatabase {database.path()};
    auto hasReport = [&] { return !reportDatabase.readPendingReports().empty(); };
    Threads::runEventLoopUntil(hasReport, libraryReportWaitLimit);
    auto reports = reportDatabase.readPendingReports();
    check(reports.size() == 1);

    if (reports.empty())
        return std::nullopt;

    return reports[0];
}

bool isLibraryFrame(const StackFrame& frame)
{
    return FilePath {frame.modulePath}.fileName() == getLibraryFileName();
}

std::optional<StackFrame> findLibraryFrame(const Vector<StackFrame>& frames)
{
    auto found = std::ranges::find_if(frames, isLibraryFrame);

    if (found == frames.end())
        return std::nullopt;

    return *found;
}

void printFrames(const Vector<StackFrame>& frames)
{
    for (const auto& frame: frames)
        Console::printError("  " + frame.modulePath + " " + frame.symbol + " "
                            + frame.moduleUuid);
}

// What a symbolizer needs: the library by name, where it was loaded, its
// debug id, and the function already resolved.
void checkFrameInLibrary(const Vector<StackFrame>& frames, std::string_view function)
{
    auto frame = findLibraryFrame(frames);

    if (!frame || frame->symbol.find(function) == std::string::npos)
        printFrames(frames);

    check(frame.has_value());

    if (!frame)
        return;

    check(frame->symbol.find(function) != std::string::npos);
    check(frame->moduleBase != 0);
    check(!frame->moduleUuid.empty());
}

void checkNullPointerInLibrary(std::string_view thread)
{
    auto report = crashInLoadedLibrary(libraryFaultFunction, thread);

    if (report)
        checkFrameInLibrary(report->crashingThread.frames, libraryFaultFunction);
}

auto tNullPointerMain =
    test("CrashReporting/nullPointerInLoadedLibraryIsReportedOnMainThread") = []
{ checkNullPointerInLibrary("main"); };

auto tNullPointerBackground =
    test("CrashReporting/nullPointerInLoadedLibraryIsReportedOnBackgroundThread") =
        [] { checkNullPointerInLibrary("background"); };

// A throw in the library is the matrix's uncaught exception on this lane, so it
// has the same limits. On macOS eacp records throw sites through the
// __cxa_throw in the app's own image, which a separately linked library does
// not call, so there is no throw-site record and no terminateFrames; the throw
// is still on the stack, because an exception nothing catches terminates
// inside __cxa_throw before any unwinding.
Faults::ExpectedOutcome getUncaughtExceptionOutcome(Faults::ThreadChoice thread)
{
    auto outcome =
        Faults::findExpectedOutcome("terminate", thread, Faults::getCurrentLane());
    return outcome.value_or(Faults::ExpectedOutcome {});
}

void checkUncaughtExceptionInLibrary(Faults::ThreadChoice thread)
{
    auto expected = getUncaughtExceptionOutcome(thread);
    auto report = crashInLoadedLibrary(libraryThrowFunction,
                                       Faults::getThreadChoiceName(thread));

    if (!report)
        return;

    check(report->reason == expected.reason);

    if (report->reason == "std::terminate")
        check(report->message.find(libraryThrownMessage) != std::string::npos);

    if (Platform::isMac())
        check(report->terminateFrames.empty());

    if (expected.stackReachesFault)
        checkFrameInLibrary(report->crashingThread.frames, libraryThrowFunction);
}

std::string getUncaughtExceptionBehaviour(Faults::ThreadChoice thread)
{
    if (Platform::isMac())
        return "IsReportedFromTheTerminateStack";

    if (getUncaughtExceptionOutcome(thread).status
        == Faults::OutcomeStatus::reportedWithLimits)
    {
        return "IsReportedWithLimits";
    }

    return "IsReportedWithItsThrowSite";
}

std::string getUncaughtExceptionTestName(Faults::ThreadChoice thread,
                                         std::string_view threadName)
{
    return "CrashReporting/uncaughtExceptionInLoadedLibrary"
           + getUncaughtExceptionBehaviour(thread) + "On" + std::string {threadName}
           + "Thread";
}

bool registerUncaughtExceptionCases()
{
    using Faults::ThreadChoice;
    static auto mainName = getUncaughtExceptionTestName(ThreadChoice::main, "Main");
    static auto backgroundName =
        getUncaughtExceptionTestName(ThreadChoice::background, "Background");
    test(mainName, [] { checkUncaughtExceptionInLibrary(ThreadChoice::main); });
    test(backgroundName,
         [] { checkUncaughtExceptionInLibrary(ThreadChoice::background); });
    return true;
}

[[maybe_unused]] const auto uncaughtExceptionCasesRegistered =
    registerUncaughtExceptionCases();
} // namespace LoadedLibraryTests
