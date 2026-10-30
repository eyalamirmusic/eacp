#pragma once

#include "CrashReport.h"

#include <eacp/Core/Utils/Containers.h>

#include <functional>

namespace eacp::CrashReporting
{
// What an app does with its crashes. One is registered per app with
// EACP_REGISTER_CRASH_HANDLER, and the reporter makes its own instances.
// A std::exception a callback throws is logged and dropped.
struct CrashHandler
{
    virtual ~CrashHandler() = default;

    // Called once a report is on disk; report.kind says which and where:
    // - crash: in the handler process, the app already gone, so it may open a
    //   window or upload and take as long as it likes; the handler exits when it
    //   returns. Apps::commandLineArgs() there holds Options::handlerArguments.
    // - hang: in the handler process, the app still running; return promptly,
    //   since the handler resumes watching after (a window is its own process).
    // - event: in the app, on the thread that called captureEvent.
    // Typically: show the report, or send it to your service.
    virtual void reportWritten([[maybe_unused]] const CrashReport& report) {}

    // Called in the app on the message thread, after start(), once per report
    // still in pending/ (crashes, hangs and events from earlier runs, and folders
    // a capture left without report.json), on every launch until
    // markReportHandled. Typically: start an upload asynchronously, then mark it
    // handled when it succeeds; never block the loop here.
    virtual void pendingReportFound([[maybe_unused]] const CrashReport& report) {}

    // Called once at startup, on the message thread, when the handler process
    // could not start, so crashes in this run will not be reported. `reason` is
    // a sentence for logging, e.g. that the handler did not start within N ms.
    // Typically: log it or send it to analytics.
    virtual void handlerUnavailable([[maybe_unused]] const std::string& reason) {}

    // Called on the next launch, on the message thread, when the previous run
    // ended without a clean exit and left no crash report (the handler was gone,
    // or the process was killed). Typically: count it as an unexplained exit.
    virtual void unreportedExit([[maybe_unused]] const AppSession& previousSession)
    {
    }
};

using CrashHandlerFactory = std::function<OwningPointer<CrashHandler>()>;

// A plain CrashHandler, which does nothing, when none was registered.
OwningPointer<CrashHandler> createRegisteredCrashHandler();

namespace Detail
{
struct CrashHandlerRegistration
{
    explicit CrashHandlerRegistration(CrashHandlerFactory factory);
};
} // namespace Detail
} // namespace eacp::CrashReporting

// Named by line rather than by type, so a qualified type name works.
#define EACP_CRASH_HANDLER_CONCATENATE(first, second) first##second
#define EACP_CRASH_HANDLER_REGISTRATION_NAME(line)                                  \
    EACP_CRASH_HANDLER_CONCATENATE(eacpCrashHandlerRegistration, line)

#define EACP_REGISTER_CRASH_HANDLER(HandlerType)                                    \
    static const auto EACP_CRASH_HANDLER_REGISTRATION_NAME(__LINE__) =              \
        ::eacp::CrashReporting::Detail::CrashHandlerRegistration                    \
    {                                                                               \
        [] { return ::eacp::makeOwned<HandlerType>(); }                             \
    }
