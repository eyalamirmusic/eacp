#include "CrashingProcess.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/TemporaryFile.h>
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

std::optional<CrashReport> crashInLoadedLibrary(std::string_view fault,
                                                std::string_view thread)
{
    auto database = TemporaryFile {};
    auto options = Tests::makeCrashingProcessOptions({"--crash-in-library",
                                                      std::string {fault},
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
    auto report = crashInLoadedLibrary("nullptr", thread);

    if (report)
        checkFrameInLibrary(report->crashingThread.frames, libraryFaultFunction);
}

auto tNullPointerMain =
    test("CrashReporting/nullPointerInLoadedLibraryIsReportedOnMainThread") = []
{ checkNullPointerInLibrary("main"); };

auto tNullPointerBackground =
    test("CrashReporting/nullPointerInLoadedLibraryIsReportedOnBackgroundThread") =
        [] { checkNullPointerInLibrary("background"); };

// Windows walks the thrown exception's own stack. On macOS eacp records throw
// sites through the __cxa_throw in the app's own image, which a separately
// linked library does not call, so there is no throw-site record and no
// terminateFrames; the throw is still on the stack because an exception
// nothing catches terminates inside __cxa_throw, before any unwinding.
void checkUncaughtExceptionInLibrary(std::string_view thread)
{
    auto report = crashInLoadedLibrary("terminate", thread);

    if (!report)
        return;

    check(report->reason == "std::terminate");
    check(report->message.find(libraryThrownMessage) != std::string::npos);

    if (Platform::isMac())
        check(report->terminateFrames.empty());

    checkFrameInLibrary(report->crashingThread.frames, libraryThrowFunction);
}

std::string getUncaughtExceptionBehaviour()
{
    if (Platform::isMac())
        return "IsReportedFromTheTerminateStack";

    return "IsReportedWithItsThrowSite";
}

std::string getUncaughtExceptionTestName(std::string_view thread)
{
    auto behaviour = getUncaughtExceptionBehaviour();
    return "CrashReporting/uncaughtExceptionInLoadedLibrary" + behaviour + "On"
           + std::string {thread} + "Thread";
}

bool registerUncaughtExceptionCases()
{
    static auto mainName = getUncaughtExceptionTestName("Main");
    static auto backgroundName = getUncaughtExceptionTestName("Background");
    test(mainName, [] { checkUncaughtExceptionInLibrary("main"); });
    test(backgroundName, [] { checkUncaughtExceptionInLibrary("background"); });
    return true;
}

[[maybe_unused]] const auto uncaughtExceptionCasesRegistered =
    registerUncaughtExceptionCases();
} // namespace LoadedLibraryTests
