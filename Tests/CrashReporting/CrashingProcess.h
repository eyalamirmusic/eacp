#pragma once

#include <eacp/Core/Process/Process.h>

#include <string_view>

// CrashReportingTests relaunched as the app that crashes: --crashing-process
// starts the crash reporter and raises the fault the flags after it name.
namespace eacp::CrashReporting::Tests
{
constexpr auto crashingProcessFlag = std::string_view {"--crashing-process"};
constexpr auto crashingProcessProject =
    std::string_view {"eacp CrashReportingTests"};
constexpr auto crashingProcessModuleName = std::string_view {"CrashReportingTests"};

bool isCrashingProcessCommandLine(int argc, char* argv[]);
int runCrashingProcess();

// This test binary with crashingProcessFlag and then the arguments given.
Processes::ProcessOptions
    makeCrashingProcessOptions(const Vector<std::string>& arguments);
} // namespace eacp::CrashReporting::Tests
