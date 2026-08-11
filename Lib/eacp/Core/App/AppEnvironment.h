#pragma once

#include "../Utils/Common.h"

#include "../Utils/Environment.h"

namespace eacp::Apps
{

// Process-wide options. Set before any Window / WebView is constructed.
struct AppEnvironment
{
    // Windows are created and child views attach, but nothing becomes visible.
    bool headless = getEnvValue("EACP_HEADLESS") == "1";

    // Index 0 is the executable path, per the argv convention.
    Vector<std::string> commandLineArgs;
};

AppEnvironment& getAppEnvironment();

// Call once at startup, before app construction.
void setCommandLineArgs(int argc, char* argv[]);

// Also exports EACP_HEADLESS, so eacp copies inside plugins loaded afterwards
// inherit it.
void setHeadless(bool headless);

} // namespace eacp::Apps
