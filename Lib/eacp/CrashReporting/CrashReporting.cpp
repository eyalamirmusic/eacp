#include "CrashReporting.h"
#include "CrashReportingPlatform.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Random.h>
#include <eacp/Core/Utils/Strings.h>

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

void deliverPendingReport(const CrashReport& report)
{
    try
    {
        createRegisteredCrashHandler()->pendingReportFound(report);
    }
    catch (const std::exception& error)
    {
        Console::printError(std::string {"pendingReportFound threw: "}
                            + error.what());
    }
}

void schedulePendingReports(const ReportDatabase& database)
{
    for (const auto& report: database.readPendingReports())
        Threads::callAsync([report] { deliverPendingReport(report); });
}

Detail::HandlerLaunch makeHandlerLaunch(const Options& options)
{
    auto launch = Detail::HandlerLaunch {};
    launch.executable = Files::executablePath();
    launch.databaseDirectory = options.databaseDirectory;
    launch.channelName = options.channelName;

    if (launch.channelName.empty())
        launch.channelName = createDefaultChannelName();
    launch.handlerArguments = options.handlerArguments;
    launch.startTimeout = options.handlerStartTimeout;

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
    }

    auto launched = Detail::launchHandlerProcess(launch, state);
    Detail::installCrashHandlers(state);

    schedulePendingReports(database);
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

    Detail::uninstallCrashHandlers();
    Detail::stopHandlerProcess(session.sharedState->get());
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
            Detail::addSharedBreadcrumb(
                state, breadcrumb, getMillisecondsSinceEpoch());
        });
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
