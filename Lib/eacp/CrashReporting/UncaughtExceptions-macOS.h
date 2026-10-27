#pragma once

#include <string_view>

namespace eacp::CrashReporting::Detail
{
// Records why the app is about to abort, so the SIGABRT that follows reports
// it rather than its own name. Joins name and detail without allocating.
void recordCrashMessage(std::string_view reason,
                        std::string_view name,
                        std::string_view detail);

void installUncaughtExceptionRecorder();
void uninstallUncaughtExceptionRecorder();
} // namespace eacp::CrashReporting::Detail
