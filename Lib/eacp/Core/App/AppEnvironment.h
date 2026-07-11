#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/Environment.h>
#include <string>

namespace eacp::Apps
{

// Process-wide options. Set before any Window / WebView is constructed
// (typically in main(), before Apps::run<T>()) so app code can read them
// during construction.
struct AppEnvironment
{
    // When true, windows are created and child views attach (so a WebView
    // still loads its page and runs JS) but never become visible. Lets test
    // binaries run on CI machines with no active windowing session.
    // Defaults from EACP_HEADLESS so every eacp copy in the process — the
    // host's and each dlopen'd plugin's — starts with the same answer; use
    // setHeadless() to change it at runtime for this copy and all copies
    // loaded afterwards.
    bool headless = getEnvValue("EACP_HEADLESS") == "1";

    // Snapshot of main()'s argc/argv, populated by setCommandLineArgs().
    // Index 0 is the executable path, per the argv convention.
    Vector<std::string> commandLineArgs;
};

AppEnvironment& getAppEnvironment();

// Copies main()'s argc/argv into commandLineArgs. Call once at startup,
// before app construction.
void setCommandLineArgs(int argc, char* argv[]);

// Sets headless for this eacp copy and exports EACP_HEADLESS, so eacp
// copies inside plugins loaded afterwards inherit it.
void setHeadless(bool headless);

} // namespace eacp::Apps
