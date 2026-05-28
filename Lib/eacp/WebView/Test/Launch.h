#pragma once

#include "AppDriver.h"
#include "Process.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace eacp::WebView::Test
{

struct LaunchOptions
{
    // Direct path to the app executable built with
    // EACP_WEBVIEW_ENABLE_TEST_SERVER=ON, e.g. the Mach-O inside
    // `WebViewTodo.app/Contents/MacOS/WebViewTodo`. The launcher does
    // not look inside .app bundles; pass the inner binary. Empty ->
    // falls back to the EACP_APP_BINARY env var.
    //
    // Ignored in attach mode (when attachUrl / EACP_RPC_URL is set
    // and bundle is empty).
    std::string bundle;

    // Attach to an already-running app instead of spawning one. Full
    // RPC endpoint, e.g. "http://127.0.0.1:8765/rpc". Empty -> falls
    // back to EACP_RPC_URL, then to defaultAttachUrl below. When
    // attached, `close()` becomes a no-op so the manually launched
    // app survives between test runs.
    std::string attachUrl;

    // Extra argv passed to the spawned child.
    std::vector<std::string> args;

    // Env vars merged onto the parent environment for the child.
    std::map<std::string, std::string> env;

    // Working directory for the child. Empty -> inherit parent's cwd.
    std::string workingDirectory;

    // Max time to wait for the EACP_RPC_PORT=<n> line on stdout.
    // First-launch on macOS (entitlement/signature checks) can be
    // slow; 10s is plenty in steady state.
    std::chrono::milliseconds startupTimeout {10000};

    // Forwarded to the AppDriver constructor.
    AppDriverOptions driverOptions;
};

// Owns the spawned (or attached) app + its driver. The dtor calls
// close() so callers don't have to think about leaks on the
// exceptional path.
struct LaunchedApp
{
    LaunchedApp() = default;
    LaunchedApp(LaunchedApp&&) = default;
    LaunchedApp& operator=(LaunchedApp&&) = default;
    LaunchedApp(const LaunchedApp&) = delete;
    LaunchedApp& operator=(const LaunchedApp&) = delete;
    ~LaunchedApp();

    // Spawn mode: SIGTERM/TerminateProcess the child and reap.
    // Attach mode: no-op. Safe to call multiple times.
    void close();

    std::unique_ptr<AppDriver> driver;

    // null in attach mode (no child was spawned).
    std::unique_ptr<Process> process;

    std::string rpcUrl;
};

// Default attach URL — agreed with TestServer.cpp's defaultRpcPort
// (8765). Lets `./MyApp` + this driver connect with zero env vars.
constexpr auto defaultAttachUrl = "http://127.0.0.1:8765/rpc";

// Spawn-or-attach launch entry point:
//   - bundle (or EACP_APP_BINARY)  -> spawn
//   - attachUrl (or EACP_RPC_URL)  -> attach
//   - neither                       -> attach to defaultAttachUrl
LaunchedApp launchApp(const LaunchOptions& options = {});

} // namespace eacp::WebView::Test
