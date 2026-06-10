#pragma once

#include "../Threads/EventLoop.h"
#include "../Utils/Common.h"
#include "AppEnvironment.h"
#include <eacp/Core/Utils/Containers.h>
#include <optional>
#include <string>

namespace eacp::Apps
{
struct AppBase
{
    virtual ~AppBase() = default;
};

template <typename T>
struct App : AppBase
{
    T app;
};

using AppHandle = OwningPointer<AppBase>;
using AppFactory = Callback;

AppHandle& getGlobalApp();
AppFactory& getAppFactory();

void destroyApp();
void quit();

// Destroys the current app instance and recreates it via run<T>()'s
// factory. Safe to call from any thread; returns immediately and the
// recreate happens on the next runloop tick.
void restart();

// Hands `url` to the OS for its registered handler (e.g. the default
// browser for http/https) — e.g. to run an OAuth login outside the
// in-app WebView. Linux has no backend yet and asserts if called.
void openExternalURL(const std::string& url);

// Controls whether the app shows a Dock icon and appears in the app
// switcher. Pass false to run as a menu-bar / tray-only app — pair with
// a Graphics::TrayIcon so it stays reachable. Call early (e.g. from the
// app struct's constructor).
//
// For a flicker-free accessory launch, also set LSUIElement in the
// bundle's Info.plist; this call then just confirms the policy at
// runtime. No-op on Windows, Linux and iOS.
void setDockIconVisible(bool visible);

struct FilePickerOptions
{
    Vector<std::string> allowedExtensions;
};

// Shows the OS's native file chooser, blocking until the user picks a file
// or cancels. Returns the chosen absolute path, or std::nullopt on cancel.
// Must be called on the UI thread. Implemented on macOS; other backends
// return std::nullopt for now.
std::optional<std::string> chooseFile(const FilePickerOptions& options = {});

// Shows the OS's native folder chooser, blocking until the user picks a
// directory or cancels. Returns the chosen absolute path, or std::nullopt
// on cancel. Must be called on the UI thread. Implemented on macOS; other
// backends return std::nullopt for now.
std::optional<std::string> chooseDirectory();

template <typename T>
void run()
{
    auto createFunc = [] { getGlobalApp().template create<App<T>>(); };
    getAppFactory() = createFunc;
    Threads::runEventLoop(createFunc);
    // The single teardown point: the app is constructed on the first loop
    // tick and destroyed here on the main thread once the loop has fully
    // exited, so no native event delivery or nested pump can still be
    // referencing the views. Apps::quit() only stops the loop.
    destroyApp();
}

// argc/argv overload — captures the command line (see commandLineArgs)
// before starting the loop, so main() is a one-liner:
// `Apps::run<MyApp>(argc, argv);`.
template <typename T>
void run(int argc, char* argv[])
{
    setCommandLineArgs(argc, argv);
    run<T>();
}

// Function overload — runs `func` once on the first loop tick and quits when
// it returns, for app-shaped work with no app state to keep alive (a compute
// job, a test runner). The loop is fully bootstrapped while `func` runs, so
// timers fire and nested pumps (runEventLoopFor / runEventLoopUntil) work.
// Use run<T>() when state must outlive a single call (windows, tray icons).
inline void run(const Callback& func)
{
    Threads::runEventLoop(
        [&func]
        {
            func();
            quit();
        });
}

} // namespace eacp::Apps
