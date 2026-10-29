#include "CrashReporting.h"
#include "CrashReportingPlatform.h"
#include "ThreadWatch.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Random.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Utils/Time.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <type_traits>

namespace eacp::CrashReporting
{
namespace
{
constexpr auto handlerFlagIndex = 1;
constexpr auto handlerDatabaseIndex = 2;
constexpr auto handlerChannelIndex = 3;
constexpr auto handlerSeparatorIndex = 4;
constexpr auto crashesFolderName = "Crashes";
constexpr auto channelSuffixLimit = 1000000000u;

struct CrashReportingSession
{
    std::mutex mutex;
    bool started = false;
    bool stopped = false;
    bool handlerProcess = false;
    bool crashedLastRun = false;
    Options options;
    std::optional<std::reference_wrapper<Detail::SharedState>> sharedState;
    std::atomic<bool> heartbeatRunning {false};

    // Set once sharedState is, for the lock-free reads a ThreadWatch heartbeat makes.
    std::atomic<bool> threadsWatched {false};
};

CrashReportingSession& getSession()
{
    static auto session = CrashReportingSession {};
    return session;
}

bool isHandlerCommandLine(int argc, char* argv[])
{
    return argc > handlerChannelIndex && argv[handlerFlagIndex] == handlerFlag;
}

void storeHandlerCommandLine(int argc, char* argv[])
{
    auto commandLine = Vector<char*> {};
    commandLine.add(argv[0]);

    for (auto index = handlerSeparatorIndex + 1; index < argc; ++index)
        commandLine.add(argv[index]);

    Apps::setCommandLineArgs(commandLine.size(), commandLine.data());
}

std::string getAppIdentity()
{
    auto appName = std::string {Platform::getAppName()};

    if (appName.empty())
        appName = Files::executablePath().stem();

    auto company = std::string {Platform::getCompanyName()};

    if (company.empty())
        return appName;

    return company + "." + appName;
}

void installStackOverflowReporting(StackOverflowReporting reporting)
{
    if (reporting == StackOverflowReporting::off)
        return;

    Detail::installStackOverflowStackOnCurrentThread();

    if (reporting == StackOverflowReporting::everyThread)
        Detail::setStackOverflowStackOnNewThreads(true);
}

// The handler is the app's own executable, so its build is the app's.
std::string getArchitectureName()
{
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_WIN32) && defined(_M_X64)
    return "x64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

void setDefaultMetadata(Detail::SharedState& state, const Options& options)
{
    auto& metadata = state.metadata;
    Detail::writeSharedEntry(state, metadata, "appName", Platform::getAppName());
    Detail::writeSharedEntry(
        state, metadata, "appVersion", Platform::getAppVersion());
    Detail::writeSharedEntry(state,
                             metadata,
                             "os",
                             getPlatformName() + " "
                                 + Detail::getOperatingSystemVersion());
    Detail::writeSharedEntry(state, metadata, "arch", getArchitectureName());
    Detail::writeSharedEntry(state, metadata, "release", options.release);
    Detail::writeSharedEntry(state, metadata, "environment", options.environment);
    Detail::writeSharedEntry(state, metadata, "dist", options.dist);

    for (const auto& attachment: options.attachments)
        Detail::writeSharedAttachment(state, attachment);
}

template <typename WriteFunction>
auto writeToSharedState(WriteFunction write)
{
    auto& session = getSession();
    auto lock = std::lock_guard {session.mutex};
    using Result = decltype(write(session.sharedState->get()));

    if (!session.started || !session.sharedState)
    {
        if constexpr (std::is_void_v<Result>)
            return;
        else
            return Result {};
    }

    return write(session.sharedState->get());
}

template <typename HandlerCall>
void scheduleHandlerCall(std::string_view name, HandlerCall call)
{
    Threads::callAsync(
        [name, call]
        {
            try
            {
                call(*createRegisteredCrashHandler());
            }
            catch (const std::exception& error)
            {
                Console::printError(std::string {name} + " threw: " + error.what());
            }
        });
}

void schedulePendingReports(const Vector<CrashReport>& reports)
{
    for (const auto& report: reports)
    {
        scheduleHandlerCall("pendingReportFound",
                            [report](CrashHandler& handler)
                            { handler.pendingReportFound(report); });
    }
}

bool isCrashSince(const CrashReport& report, const AppSession& session)
{
    return report.kind == ReportKind::crash && report.timestamp >= session.startTime;
}

void scheduleUnreportedExit(const ReportDatabase& database,
                            const Vector<CrashReport>& reports)
{
    auto previous = database.readSessionMarker();

    if (!previous)
        return;

    auto reported = std::ranges::any_of(reports,
                                        [&](const CrashReport& report)
                                        { return isCrashSince(report, *previous); });

    if (reported)
        return;

    scheduleHandlerCall("unreportedExit",
                        [session = *previous](CrashHandler& handler)
                        { handler.unreportedExit(session); });
}

AppSession makeCurrentSession()
{
    auto startTime = Time::sinceEpoch();
    auto session = AppSession {};
    session.processId = Detail::getCurrentProcessId();
    session.startTime = startTime.count;
    session.startTimeText = Time::formatIso8601(startTime);
    return session;
}

void clearSessionMarkerAtExit()
{
    getDatabase().clearSessionMarker();
}

std::string describeUnavailableHandler(const Detail::HandlerLaunch& launch)
{
    return "the handler process " + launch.executable.str()
           + " did not start within " + std::to_string(launch.startTimeout.count)
           + " ms";
}

Detail::SharedWatchedThread& getMainThreadWatch(Detail::SharedHangInfo& hang)
{
    return hang.threads[Detail::mainThreadSlot];
}

void writeHeartbeat(Detail::SharedHangInfo& hang)
{
    Detail::sendWatchedThreadHeartbeat(getMainThreadWatch(hang));
}

// Each heartbeat posts the next through callAfter rather than a Timer: Windows
// delays WM_TIMER behind every other message and coalesces it by up to a
// second, which an idle loop would be reported as a hang for.
void scheduleHeartbeat(CrashReportingSession& session, Detail::SharedHangInfo& hang)
{
    auto interval = session.options.heartbeatInterval;

    Threads::callAfter(interval,
                       [&session, &hang]
                       {
                           if (!session.heartbeatRunning)
                               return;

                           writeHeartbeat(hang);
                           scheduleHeartbeat(session, hang);
                       });
}

// Before the handler launches, since on Windows it reads these to decide how
// often it wakes.
void writeHangSettings(Detail::SharedState& state, const Options& options)
{
    auto& hang = state.hang;
    hang.thresholdMilliseconds = Detail::countMilliseconds(options.hangThreshold);
    hang.pollIntervalMilliseconds =
        Detail::countMilliseconds(options.heartbeatInterval);
    Detail::watchMainThread(hang, Detail::getCurrentThreadId());
    Detail::recordThreadForSampling(getMainThreadWatch(hang));
}

OwningPointer<Threads::ThreadWatchScope> openThreadWatch(std::string_view name,
                                                         Time::MS threshold)
{
    return makeOwned<ThreadWatch>(name, threshold);
}

void armHangDetection(CrashReportingSession& session, Detail::SharedState& state)
{
    auto& hang = state.hang;

    if (hang.thresholdMilliseconds == 0 || hang.pollIntervalMilliseconds == 0)
        return;

    session.heartbeatRunning = true;
    session.threadsWatched = true;
    scheduleHeartbeat(session, hang);
    Detail::startHangWatchdog(state);
    Threads::setThreadWatchFactory(openThreadWatch);
}

Detail::HandlerLaunch makeHandlerLaunch(const Options& options)
{
    auto launch = Detail::HandlerLaunch {};
    launch.executable = options.handlerExecutable;

    if (launch.executable.empty())
        launch.executable = Files::executablePath();

    launch.databaseDirectory = options.databaseDirectory;
    launch.channelName = options.channelName;

    if (launch.channelName.empty())
        launch.channelName = createDefaultChannelName();
    launch.handlerArguments = options.handlerArguments;
    launch.startTimeout = options.handlerStartTimeout;
    launch.captureMachExceptions = options.captureMachExceptions;
    launch.shouldListCrashHelperWithWindowsErrorReporting =
        options.registerCrashHelperWithWindowsErrorReporting;

    return launch;
}
} // namespace

FilePath getDefaultDatabaseDirectory()
{
    return FilePath::appSupportDirectory() / crashesFolderName;
}

std::string createDefaultChannelName()
{
    auto random = Random {};
    auto suffix = random.get<unsigned int>(0u, channelSuffixLimit);

    return getAppIdentity() + ".crash."
           + std::to_string(Detail::getCurrentProcessId()) + "."
           + std::to_string(suffix);
}

std::optional<int> runHandlerIfRequested(int argc, char* argv[])
{
    if (!isHandlerCommandLine(argc, argv))
        return std::nullopt;

    getSession().handlerProcess = true;
    storeHandlerCommandLine(argc, argv);

    auto arguments = Detail::HandlerArguments {};
    arguments.databaseDirectory = FilePath {argv[handlerDatabaseIndex]};
    arguments.channel = argv[handlerChannelIndex];

    return Detail::runHandlerProcess(arguments);
}

bool isHandlerProcess()
{
    return getSession().handlerProcess;
}

bool start(const Options& options)
{
    auto& session = getSession();

    {
        auto lock = std::lock_guard {session.mutex};

        if (session.started || session.stopped || session.handlerProcess)
            return false;

        session.started = true;
        session.options = options;
    }

    auto database = ReportDatabase {options.databaseDirectory};
    Files::createDirectories(database.getPendingDirectory());
    session.crashedLastRun = database.hasCrashedLastRun();

    auto launch = makeHandlerLaunch(options);
    auto& state = Detail::createSharedState(launch);

    {
        auto lock = std::lock_guard {session.mutex};
        session.sharedState = state;
        setDefaultMetadata(state, options);
        writeHangSettings(state, options);
    }

    auto launched = Detail::launchHandlerProcess(launch, state);

    Detail::installCrashHandlers(state);
    installStackOverflowReporting(options.stackOverflowReporting);

    if (launched)
    {
        armHangDetection(session, state);
    }
    else
    {
        scheduleHandlerCall(
            "handlerUnavailable",
            [reason = describeUnavailableHandler(launch)](CrashHandler& handler)
            { handler.handlerUnavailable(reason); });
    }

    auto pendingReports = database.readPendingReports();
    scheduleUnreportedExit(database, pendingReports);
    database.writeSessionMarker(makeCurrentSession());
    std::atexit(clearSessionMarkerAtExit);
    schedulePendingReports(pendingReports);
    return launched;
}

bool isStarted()
{
    return getSession().started;
}

bool isHandlerRunning()
{
    return isStarted() && Detail::isHandlerProcessRunning();
}

void stop()
{
    auto& session = getSession();
    auto lock = std::lock_guard {session.mutex};

    if (!session.started || !session.sharedState)
        return;

    Threads::setThreadWatchFactory({});
    Detail::stopHangWatchdog();
    session.heartbeatRunning = false;
    session.threadsWatched = false;
    Detail::uninstallCrashHandlers();
    Detail::setStackOverflowStackOnNewThreads(false);
    Detail::stopHandlerProcess(session.sharedState->get());
    ReportDatabase {session.options.databaseDirectory}.clearSessionMarker();
    session.started = false;
    session.stopped = true;
}

bool setTag(std::string_view key, std::string_view value)
{
    return writeToSharedState(
        [&](Detail::SharedState& state)
        { return Detail::writeSharedEntry(state, state.tags, key, value); });
}

bool removeTag(std::string_view key)
{
    return writeToSharedState(
        [&](Detail::SharedState& state)
        { return Detail::removeSharedEntry(state, state.tags, key); });
}

bool setExtra(std::string_view key, std::string_view value)
{
    return writeToSharedState(
        [&](Detail::SharedState& state)
        { return Detail::writeSharedEntry(state, state.extra, key, value); });
}

bool setUser(const User& user)
{
    return writeToSharedState([&](Detail::SharedState& state)
                              { return Detail::writeSharedUser(state, user); });
}

void clearUser()
{
    writeToSharedState([](Detail::SharedState& state)
                       { Detail::clearSharedUser(state); });
}

bool addAttachment(const FilePath& path)
{
    return writeToSharedState(
        [&](Detail::SharedState& state)
        { return Detail::writeSharedAttachment(state, path); });
}

void addBreadcrumb(const Breadcrumb& breadcrumb)
{
    writeToSharedState(
        [&](Detail::SharedState& state)
        {
            Detail::addSharedBreadcrumb(state, breadcrumb, Time::sinceEpoch().count);
        });
}

void pauseHangDetection()
{
    writeToSharedState(
        [](Detail::SharedState& state)
        { Detail::pauseWatchedThread(getMainThreadWatch(state.hang)); });
}

void resumeHangDetection()
{
    writeToSharedState(
        [](Detail::SharedState& state)
        { Detail::resumeWatchedThread(getMainThreadWatch(state.hang)); });
}

namespace
{
void deliverEvent(const CrashReport& report)
{
    try
    {
        createRegisteredCrashHandler()->eventReported(report);
    }
    catch (const std::exception& error)
    {
        Console::printError(std::string {"eventReported threw: "} + error.what());
    }
}
} // namespace

std::string captureEvent(const Event& event)
{
    constexpr auto framesOfCaptureEvent = 1;
    auto frames = Detail::captureCurrentStack(framesOfCaptureEvent);
    auto database = getDatabase();

    auto report = writeToSharedState(
        [&](Detail::SharedState& state)
        {
            auto written =
                Detail::beginHandlerReport(database, state, CaptureSource::app);
            written.kind = ReportKind::event;
            written.level = event.level;
            written.reason = event.reason;
            written.message = event.message;
            written.faultAddress = 0;
            written.crashingThread.threadId = Detail::getCurrentThreadId();
            written.crashingThread.frames = frames;

            if (!Detail::finishHandlerReport(database, written, state))
                return std::optional<CrashReport> {};

            return std::optional {written};
        });

    if (!report)
        return {};

    deliverEvent(*report);
    return report->id;
}

std::string captureMessage(std::string_view message, Level level)
{
    return captureEvent({messageEventReason, std::string {message}, level});
}

std::string captureException(const std::exception& exception)
{
    auto message = Detail::getTypeName(typeid(exception)) + ": " + exception.what();
    return captureEvent({exceptionEventReason, message, Level::error});
}

bool crashedLastRun()
{
    return getSession().crashedLastRun;
}

void clearCrashedLastRun()
{
    getSession().crashedLastRun = false;
    getDatabase().clearCrashedLastRun();
}

bool markReportHandled(std::string_view id)
{
    return getDatabase().markReportHandled(id,
                                           getSession().options.maxHandledReports);
}

ReportDatabase getDatabase()
{
    return ReportDatabase {getSession().options.databaseDirectory};
}
} // namespace eacp::CrashReporting

namespace eacp::CrashReporting::Detail
{
std::uint64_t countMilliseconds(Time::MS duration)
{
    return static_cast<std::uint64_t>(std::max<std::int64_t>(duration.count, 0));
}

std::optional<std::reference_wrapper<SharedHangInfo>> findWatchedHangInfo()
{
    auto& session = getSession();

    if (!session.threadsWatched.load(std::memory_order_acquire))
        return std::nullopt;

    return session.sharedState->get().hang;
}
} // namespace eacp::CrashReporting::Detail
