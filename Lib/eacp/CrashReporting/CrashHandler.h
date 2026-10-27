#pragma once

#include "CrashReport.h"

#include <functional>
#include <memory>

namespace eacp::CrashReporting
{
// What an app does with its crashes. One is registered per app with
// EACP_REGISTER_CRASH_HANDLER, and the reporter makes its own instances:
//
// crashReported runs in the handler process, once the report is on disk and the
// crashed app is gone, so it may allocate, open windows and take as long as it
// likes; the handler exits when it returns. Apps::commandLineArgs() there is
// the executable followed by Options::handlerArguments.
//
// pendingReportFound runs in the app on the message thread, after start() has
// returned and the event loop has started, once per report still in pending/ on
// each launch until markReportHandled moves it to handled/. That includes a
// folder a capture left without its report.json. It runs on the loop, so long
// work (an upload) is started asynchronously from it, never awaited in it.
struct CrashHandler
{
    virtual ~CrashHandler() = default;

    virtual void crashReported(const CrashReport&) {}
    virtual void pendingReportFound(const CrashReport&) {}
};

using CrashHandlerFactory = std::function<std::unique_ptr<CrashHandler>()>;

// A plain CrashHandler, which does nothing, when none was registered.
std::unique_ptr<CrashHandler> createRegisteredCrashHandler();

namespace Detail
{
struct CrashHandlerRegistration
{
    explicit CrashHandlerRegistration(CrashHandlerFactory factory);
};
} // namespace Detail
} // namespace eacp::CrashReporting

#define EACP_REGISTER_CRASH_HANDLER(HandlerType)                                    \
    static const auto eacpCrashHandlerRegistration##HandlerType =                   \
        ::eacp::CrashReporting::Detail::CrashHandlerRegistration                    \
    {                                                                               \
        [] { return std::make_unique<HandlerType>(); }                              \
    }
