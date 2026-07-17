#pragma once

#include "../Platform/Platform.h"
#include "../Threads/EventLoop.h"
#include "../Utils/Common.h"
#include "AppEnvironment.h"

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

// The value run<T>() returns once the loop has unwound — main()'s exit
// code. Defaults to 0 and resets on each run(). Safe to call from any
// thread; quit(returnValue) is the usual way to set it.
void setReturnValue(int returnValue);
int getReturnValue();

void quit();

// Sets the return value and quits, so with
// `int main() { return Apps::run<App>(); }` the process exits with
// `returnValue`.
void quit(int returnValue);

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

// macOS: called when the user reactivates the app (Dock icon click) while
// it has no visible windows — applicationShouldHandleReopen:. A window
// hidden via WindowOptions::hidesOnClose uses this to come back:
// setReopenHandler([&] { window.setVisible(true); }). Unset, reopen falls
// through to the system default. Never fires on other platforms.
void setReopenHandler(const Callback& handler);

// Internal: the handler above, invoked by the platform's app delegate.
const Callback& getReopenHandler();

// True when this process's executable carries a distribution code signature:
// Developer ID or Apple-issued (App Store / system) on macOS, an embedded
// Authenticode signature on Windows, no development provisioning profile on
// iOS. Local builds — unsigned, linker ad-hoc signed, or Xcode
// development-signed — return false, so this doubles as a "running a released
// build?" check. Deliberately ignores certificate expiry and revocation: the
// answer must stay stable offline and over time, and a false negative would
// silently flip a released install into dev behaviour. Linux has no signing
// convention and returns false.
bool isDistributionSigned();

struct FilePickerOptions
{
    Vector<std::string> allowedExtensions;
};

// Shows the OS's native file chooser, blocking until the user picks a file
// or cancels. Returns the chosen absolute path, or std::nullopt on cancel.
// Must be called on the UI thread. Implemented on macOS and Windows; Linux
// returns std::nullopt for now.
std::optional<std::string> chooseFile(const FilePickerOptions& options = {});

// Shows the OS's native folder chooser, blocking until the user picks a
// directory or cancels. Returns the chosen absolute path, or std::nullopt
// on cancel. Must be called on the UI thread. Implemented on macOS and
// Windows; Linux returns std::nullopt for now.
std::optional<std::string> chooseDirectory();

// True when this copy's app was started by run<T>() from inside a dynamic
// library — it rides the host executable's loop instead of owning one, and
// its quit() stops that loop rather than its own (see Apps::quit).
bool isRunningAsPlugin();

namespace Detail
{
// run<T>()'s dynamic-library path: marks this copy as a plugin-hosted app
// and schedules its construction onto the loop the host runs.
void runAsPlugin(const AppFactory& createFunc);
} // namespace Detail

template <typename T>
int run()
{
    auto createFunc = [] { getGlobalApp().template create<App<T>>(); };
    getAppFactory() = createFunc;

    // In a dynamic library the process executable owns the root loop —
    // running one here would fight it (or, under a foreign host, steal its
    // app delegate). Schedule the app onto the host's loop and return
    // immediately; the app is destroyed when the library's image is torn
    // down, after the host's loop has exited.
    if (Platform::isDLL())
    {
        Detail::runAsPlugin(createFunc);
        return 0;
    }

    setReturnValue(0);
    Threads::runEventLoop(createFunc);
    // The single teardown point: the app is constructed on the first loop
    // tick and destroyed here on the main thread once the loop has fully
    // exited, so no native event delivery or nested pump can still be
    // referencing the views. Apps::quit() only stops the loop.
    destroyApp();
    return getReturnValue();
}

// argc/argv overload — captures the command line (see commandLineArgs)
// before starting the loop, so main() is a one-liner:
// `return Apps::run<MyApp>(argc, argv);`.
template <typename T>
int run(int argc, char* argv[])
{
    setCommandLineArgs(argc, argv);
    return run<T>();
}

// Function overload — runs `func` once on the first loop tick and quits when
// it returns, for app-shaped work with no app state to keep alive (a compute
// job, a test runner). The loop is fully bootstrapped while `func` runs, so
// timers fire and nested pumps (runEventLoopFor / runEventLoopUntil) work.
// Use run<T>() when state must outlive a single call (windows, tray icons).
int run(const Callback& func);

} // namespace eacp::Apps
