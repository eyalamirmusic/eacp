#include "Launch.h"

#include <cstdlib>
#include <regex>
#include <stdexcept>

namespace eacp::WebView::Test
{

namespace
{

std::string envOr(const char* name, std::string fallback)
{
    if (auto* v = std::getenv(name); v != nullptr)
        return v;
    return fallback;
}

std::optional<std::string> envValue(const char* name)
{
    if (auto* v = std::getenv(name); v != nullptr)
        return std::string(v);
    return std::nullopt;
}

// Best-effort grep of the accumulated stdout buffer for the well-known
// announcement line written by TestServer.cpp on listen() success.
std::optional<int> findAnnouncedPort(const std::string& stdoutBuf)
{
    static const auto re = std::regex {R"(EACP_RPC_PORT=(\d+))"};
    auto match = std::smatch {};
    if (!std::regex_search(stdoutBuf, match, re))
        return std::nullopt;
    return std::stoi(match[1].str());
}

LaunchedApp attach(const std::string& rpcUrl, const LaunchOptions& options)
{
    auto driver = std::make_unique<AppDriver>(rpcUrl, options.driverOptions);

    // Health probe — confirms the app is reachable up front so the
    // first real test isn't the one to discover a typo'd URL.
    (void) driver->evaluate("1");

    auto launched = LaunchedApp {};
    launched.driver = std::move(driver);
    launched.process = nullptr;
    launched.rpcUrl = rpcUrl;
    return launched;
}

LaunchedApp spawnAndLaunch(const std::string& bundle, const LaunchOptions& options)
{
    auto procOpts = ProcessOptions {};
    procOpts.args = options.args;
    procOpts.workingDirectory = options.workingDirectory;
    // Override EACP_RPC_PORT=0 so each spawned child takes an ephemeral
    // port — the app's built-in default (8765) is for solo manual
    // launches and would collide across parallel test runs. We
    // discover the actual port from the child's stdout regardless of
    // what we asked for.
    procOpts.env["EACP_RPC_PORT"] = "0";
    for (auto& [k, v]: options.env)
        procOpts.env[k] = v;

    auto process = std::make_unique<Process>(bundle, procOpts);

    try
    {
        auto deadline = std::chrono::steady_clock::now() + options.startupTimeout;
        auto port = std::optional<int> {};

        while (std::chrono::steady_clock::now() < deadline)
        {
            port = findAnnouncedPort(process->stdoutBuffer());
            if (port)
                break;

            if (process->hasExited())
                throw std::runtime_error(
                    "launchApp: app exited before announcing port "
                    "(exit=" + std::to_string(process->exitCode())
                    + ")\nstdout: " + process->stdoutBuffer()
                    + "\nstderr: " + process->stderrBuffer());

            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                break;
            process->pollOutput(std::min(remaining,
                                         std::chrono::milliseconds {200}));
        }

        if (!port)
            throw std::runtime_error("launchApp: timed out after "
                                     + std::to_string(options.startupTimeout.count())
                                     + "ms waiting for EACP_RPC_PORT=<n> on "
                                     "stdout.\nstdout so far: "
                                     + process->stdoutBuffer()
                                     + "\nstderr so far: "
                                     + process->stderrBuffer());

        auto url = "http://127.0.0.1:" + std::to_string(*port) + "/rpc";
        auto driver = std::make_unique<AppDriver>(url, options.driverOptions);

        // Health probe — the port line means listen() returned but
        // the dispatcher's worker pool may not have a thread on hand
        // yet for the very first request. One round-trip warms it up.
        (void) driver->evaluate("1");

        auto launched = LaunchedApp {};
        launched.driver = std::move(driver);
        launched.process = std::move(process);
        launched.rpcUrl = std::move(url);
        return launched;
    }
    catch (...)
    {
        process->terminate();
        throw;
    }
}

} // namespace

void LaunchedApp::close()
{
    if (process)
    {
        process->terminate();
        process.reset();
    }
    driver.reset();
}

LaunchedApp::~LaunchedApp()
{
    close();
}

LaunchedApp launchApp(const LaunchOptions& options)
{
    // Spawn if the caller (or env) named a binary. Otherwise attach —
    // either to the URL the caller set, or to the well-known default.
    auto bundle = options.bundle.empty() ? envOr("EACP_APP_BINARY", "")
                                         : options.bundle;
    if (!bundle.empty())
        return spawnAndLaunch(bundle, options);

    auto attachUrl = options.attachUrl;
    if (attachUrl.empty())
    {
        if (auto fromEnv = envValue("EACP_RPC_URL"); fromEnv)
            attachUrl = *fromEnv;
        else
            attachUrl = defaultAttachUrl;
    }

    return attach(attachUrl, options);
}

} // namespace eacp::WebView::Test
