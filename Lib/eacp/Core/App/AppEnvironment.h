#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <string>

namespace eacp::Apps
{

// Process-wide options that influence framework behaviour. Set
// fields before any Window / WebView is constructed (typically
// from main(), before Apps::run<T>()) so app code can read them
// during construction.
struct AppEnvironment
{
    // When true, framework code that would otherwise force a
    // window to be shown — Cocoa's makeKeyAndOrderFront / NSApp
    // activateIgnoringOtherApps, Win32 ShowWindow — becomes a
    // no-op. Window objects still get constructed and child
    // views still attach, so WKWebView / WebView2 still load
    // their pages and run JS; they just don't render to a
    // visible surface.
    //
    // Lets test binaries run on CI machines that have no active
    // windowing session (the renderer wouldn't progress past
    // first-navigation otherwise).
    bool headless = false;

    // Snapshot of main()'s argc/argv. Populated by
    // setCommandLineArgs(); accessible from anywhere via
    // getAppEnvironment().commandLineArgs. Index 0 is the
    // executable path, matching the argv convention.
    Vector<std::string> commandLineArgs;
};

AppEnvironment& getAppEnvironment();

// Copy main()'s argc/argv into AppEnvironment::commandLineArgs.
// Call once at startup, before app construction, so the args
// are reachable from anywhere in the process.
void setCommandLineArgs(int argc, char* argv[]);

} // namespace eacp::Apps
