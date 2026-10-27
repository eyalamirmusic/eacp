#pragma once

#include <eacp/CrashReporting/CrashReport.h>
#include <eacp/Graphics/Window/Window.h>

namespace SelfDestruct
{
// Runs a window showing the report until it is closed: the handler process's
// whole life after the crash.
int runReportWindow(const eacp::CrashReporting::CrashReport& report);
} // namespace SelfDestruct
