#pragma once

#include <eacp/CrashReporting/CrashReporting.h>

namespace SelfDestruct
{
struct SelfDestructCrashHandler final : eacp::CrashReporting::CrashHandler
{
    void reportWritten(const eacp::CrashReporting::CrashReport& report) override;
    void
        pendingReportFound(const eacp::CrashReporting::CrashReport& report) override;
};
} // namespace SelfDestruct
