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

// main()'s exit code, reset on each run(). Safe to call from any thread.
void setReturnValue(int returnValue);
int getReturnValue();

void quit();
void quit(int returnValue);

// Safe to call from any thread; the recreate happens on the next runloop tick.
void restart();

// Asserts on Linux, which has no backend yet.
void openExternalURL(const std::string& url);

// Pass false for a menu-bar / tray-only app. For a flicker-free accessory
// launch also set LSUIElement in the bundle's Info.plist. No-op off macOS.
void setDockIconVisible(bool visible);

// macOS only: fires when the user reactivates the app (Dock icon click) with
// no visible windows — applicationShouldHandleReopen:.
void setReopenHandler(const Callback& handler);
const Callback& getReopenHandler();

// True when the executable carries a distribution signature: Developer ID or
// Apple-issued on macOS, Authenticode on Windows, no development provisioning
// profile on iOS. Ignores expiry and revocation. Linux always returns false.
bool isDistributionSigned();

struct FilePickerOptions
{
    Vector<std::string> allowedExtensions;
};

// Blocks on the UI thread until the user picks a file; nullopt on cancel.
// Linux returns nullopt for now.
std::optional<std::string> chooseFile(const FilePickerOptions& options = {});

struct FileSaveOptions
{
    Vector<std::string> allowedExtensions;
    std::string suggestedName;
};

// Blocks on the UI thread until the user names a file; nullopt on cancel. The
// path need not exist yet, its overwrite is already confirmed by the panel, and
// writing it is the caller's job. Linux returns nullopt for now.
std::optional<std::string> chooseSaveFile(const FileSaveOptions& options = {});

// Blocks on the UI thread until the user picks a directory; nullopt on cancel.
// Linux returns nullopt for now.
std::optional<std::string> chooseDirectory();

// True when run<T>() started this copy inside a dynamic library: it rides the
// host executable's loop, and its quit() stops that loop rather than its own.
bool isRunningAsPlugin();

namespace Detail
{
void runAsPlugin(const AppFactory& createFunc);
} // namespace Detail

template <typename T>
int run()
{
    auto createFunc = [] { getGlobalApp().template create<App<T>>(); };
    getAppFactory() = createFunc;

    // In a dynamic library the host owns the root loop, so ride it and return
    // immediately; the app is destroyed when the library's image is torn down.
    if (Platform::isDLL())
    {
        Detail::runAsPlugin(createFunc);
        return 0;
    }

    setReturnValue(0);
    Threads::runEventLoop(createFunc);
    // The single teardown point: on the main thread, once the loop has fully
    // exited, so no native event delivery can still reference the views.
    destroyApp();
    return getReturnValue();
}

template <typename T>
int run(int argc, char* argv[])
{
    setCommandLineArgs(argc, argv);
    return run<T>();
}

// Runs `func` once on the first loop tick and quits when it returns. The loop
// is fully bootstrapped while it runs, so timers and nested pumps work.
int run(const Callback& func);

} // namespace eacp::Apps
