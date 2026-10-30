#pragma once

#include <eacp/Core/Core.h>

#include <optional>
#include <string>

namespace SelfDestruct
{
constexpr auto showReportFlag = "--show-report";

struct LaunchFlags
{
    std::string crashKind;
    std::string threadName = "main";
    std::string eventKind;
    eacp::FilePath databaseDirectory;
    eacp::FilePath reportToShow;
    eacp::Vector<std::string> handlerArguments;
    eacp::Time::MS hangThreshold;
    bool pausesHangDetection = false;
    bool killsHandler = false;
    bool usesSignalsOnly = false;
    bool withoutWindowsErrorReportingHelper = false;
};

std::optional<int> parseLaunchFlags(LaunchFlags& flags);
int showReport(const eacp::FilePath& folder);
int runApp(const LaunchFlags& flags);
} // namespace SelfDestruct
