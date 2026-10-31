#include "CrashHandler.h"
#include "App.h"
#include "ReportWindow.h"

#include <eacp/Core/App/Args.h>
#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>

using namespace eacp;

namespace SelfDestruct
{
namespace
{
constexpr auto noReportWindowFlag = "--no-report-window";
}

void SelfDestructCrashHandler::reportWritten(
    const CrashReporting::CrashReport& report)
{
    if (report.kind == CrashReporting::ReportKind::event)
    {
        Console::print("SelfDestruct: event " + report.id + " (" + report.reason
                       + ": " + report.message + ")");
        return;
    }

    auto noReportWindow = false;
    auto cli = Args::Parser {"SelfDestruct crash handler"};
    cli.flag(noReportWindowFlag, noReportWindow, "write the report and exit");

    if (cli.exitCodeFor(cli.parse()) || noReportWindow)
        return;

    if (report.kind == CrashReporting::ReportKind::crash)
    {
        SelfDestruct::runReportWindow(report);
        return;
    }

    // A hung app is still running and the handler goes back to watching
    // it, so the window is a process of its own.
    auto options = Processes::ProcessOptions {};
    options.executable = Files::executablePath().str();
    options.arguments = {showReportFlag, report.folder.str()};
    options.detached = true;
    auto window = Processes::Process {options};
}

void SelfDestructCrashHandler::pendingReportFound(
    const CrashReporting::CrashReport& report)
{
    Console::print("SelfDestruct: pending report " + report.id + " (" + report.reason
                   + ")");
}
} // namespace SelfDestruct

EACP_REGISTER_CRASH_HANDLER(SelfDestruct::SelfDestructCrashHandler);
