#pragma once

#include "../Utils/Common.h"

#include "../Utils/Environment.h"

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

    // Snapshot of main()'s argc/argv as UTF-8, populated by
    // setCommandLineArgs(). Index 0 is the executable path, per the argv
    // convention.
    Vector<std::string> commandLineArgs;
};

AppEnvironment& getAppEnvironment();

// main()'s argc/argv as UTF-8, argv[0] first; a null entry is empty. On macOS,
// Linux and Android that is a copy. On Windows, where main() gets its arguments
// in the ANSI code page, a list as long as the process's command line has each
// entry that is the ANSI form of the matching process argument replaced by that
// argument's UTF-8, so characters the code page lacks survive. Any other entry,
// and a list of another length, is copied as given, so an argv the caller
// built or changed keeps its own text.
Vector<std::string> utf8Arguments(int argc, const char* const* argv);

// Stores utf8Arguments(argc, argv) in commandLineArgs. Call once at startup,
// before app construction.
void setCommandLineArgs(int argc, char* argv[]);

// getAppEnvironment().commandLineArgs: argv[0] first, empty until
// setCommandLineArgs() (or runConsole / run<T>(argc, argv)) has stored them.
const Vector<std::string>& commandLineArgs();

// Sets headless for this eacp copy and exports EACP_HEADLESS, so eacp
// copies inside plugins loaded afterwards inherit it.
void setHeadless(bool headless);

} // namespace eacp::Apps
