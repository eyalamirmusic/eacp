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
    bool handlerProcess = false;
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

void setDefaultMetadata(Detail::SharedState& state)
{
    Detail::writeSharedMetadata(state, "appName", Platform::getAppName());
    Detail::writeSharedMetadata(state, "appVersion", Platform::getAppVersion());
    Detail::writeSharedMetadata(
        state, "os", getPlatformName() + " " + Detail::getOperatingSystemVersion());
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

        if (session.started || session.handlerProcess)
            return false;

        session.started = true;
        session.options = options;
    }

    auto database = ReportDatabase {options.databaseDirectory};
    Files::createDirectories(database.getPendingDirectory());

    auto launch = makeHandlerLaunch(options);
    auto& state = Detail::createSharedState(launch);

    {
        auto lock = std::lock_guard {session.mutex};
        session.sharedState = state;
        setDefaultMetadata(state);
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

bool setMetadata(std::string_view key, std::string_view value)
{
    auto& session = getSession();
    auto lock = std::lock_guard {session.mutex};

    if (!session.sharedState)
        return false;

    return Detail::writeSharedMetadata(session.sharedState->get(), key, value);
}

bool addAttachment(const FilePath& path)
{
    auto& session = getSession();
    auto lock = std::lock_guard {session.mutex};

    if (!session.sharedState)
        return false;

    return Detail::writeSharedAttachment(session.sharedState->get(), path);
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
