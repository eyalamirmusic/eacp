#include "App.h"
#include "PersistentState.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/CrashReporting/CrashReporting.h>

using namespace eacp;

namespace
{
constexpr auto projectName = "eacp SelfDestruct";
constexpr auto demoEnvironment = "demo";
constexpr auto demoUserId = "self-destruct-user";
constexpr auto demoUserEmail = "user@example.com";
constexpr auto demoUserName = "selfdestruct";
constexpr auto windowsErrorReportingHelperFileName =
    "eacp-windows-error-reporting-helper.dll";

FilePath startSessionLog()
{
    auto log = FilePath::appCacheDirectory() / "SelfDestruct.log";
    Files::removeFile(log);
    setLogFile(log.str());
    LOG("SelfDestruct started");

    return log;
}

FilePath chooseWindowsErrorReportingHelper(const FilePath& requestedHelper)
{
    if (!requestedHelper.empty())
        return requestedHelper;

    return Files::executablePath().parentDirectory()
           / windowsErrorReportingHelperFileName;
}

void describeSessionToCrashReporting()
{
    CrashReporting::setTag("project", projectName);
    CrashReporting::setExtra("launchedFrom", Files::executablePath().str());
    CrashReporting::setUser({demoUserId, demoUserEmail, demoUserName});
    CrashReporting::addBreadcrumb({"app", "crash reporting started"});
    CrashReporting::addBreadcrumb(
        {"app", "about to offer ways to crash", CrashReporting::Level::warning});
}
} // namespace

int main(int argc, char* argv[])
{
    if (auto code = CrashReporting::runHandlerIfRequested(argc, argv))
        return *code;

    Apps::setCommandLineArgs(argc, argv);
    SelfDestruct::ignorePersistentUIState();

    auto flags = SelfDestruct::LaunchFlags {};

    if (auto code = SelfDestruct::parseLaunchFlags(flags))
        return *code;

    if (!flags.reportToShow.empty())
        return SelfDestruct::showReport(flags.reportToShow);

    auto options = CrashReporting::Options {};

    if (!flags.databaseDirectory.empty())
        options.databaseDirectory = flags.databaseDirectory;

    if (flags.usesSignalsOnly)
        options.captureMachExceptions = false;

    if (flags.withoutWindowsErrorReportingHelper)
        options.registerCrashHelperWithWindowsErrorReporting = false;

    options.handlerArguments = flags.handlerArguments;
    options.hangThreshold = flags.hangThreshold;
    options.environment = demoEnvironment;
    options.attachments = {startSessionLog()};

    if constexpr (Platform::isWindows())
    {
        if (!flags.withoutWindowsErrorReportingHelper)
            options.windowsErrorReportingHelperPath =
                chooseWindowsErrorReportingHelper(flags.windowsErrorReportingHelper);
    }

    CrashReporting::start(options);
    describeSessionToCrashReporting();

    return SelfDestruct::runApp(flags);
}
