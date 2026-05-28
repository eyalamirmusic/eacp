#pragma once

#include <eacp/Network/HTTPRpc/RpcClient.h>

#include <Miro/Miro.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eacp::WebView::Test
{

struct CallOptions
{
    // Override the driver-wide default for this one call. The server
    // caps each command at this many ms. Unset -> use AppDriverOptions
    // default; if that's also unset, the server applies its own 5s.
    std::optional<int> timeoutMs;
};

struct AppDriverOptions
{
    // Default per-command timeout. Empty -> server's 5s default.
    std::optional<int> defaultTimeoutMs;

    // Directory snapshot() writes <name>.html + <name>.png into.
    // Empty -> "<cwd>/test-results/snapshots". Created lazily on
    // first write. Names may contain forward slashes; sub-directories
    // are created as needed.
    std::string snapshotDir;
};

struct ScreenshotOptions
{
    // Per-call timeout (mirrors CallOptions for ergonomics — a
    // screenshot is one round-trip too).
    std::optional<int> timeoutMs;

    // Write the decoded PNG bytes here. Parent directories are
    // created. Empty -> do not write.
    std::string path;
};

struct ScreenshotResult
{
    std::vector<std::uint8_t> png;
    // Set only when ScreenshotOptions::path was non-empty.
    std::string path;
};

struct SnapshotOptions
{
    std::optional<int> timeoutMs;

    // Override the driver's configured snapshot directory.
    std::string dir;

    // DOM selector to scope the HTML snapshot to. Empty -> whole doc.
    std::string selector;
};

struct SnapshotResult
{
    std::string name;
    std::string dom;
    std::vector<std::uint8_t> png;
    std::string domPath;
    std::string screenshotPath;
};

// Drives a WebView app via its embedded test RPC server. Methods
// throw eacp::HTTP::Error (or std::runtime_error for client-side
// failures) on RPC failures, JS exceptions, timeouts, and missing
// selectors. Use `exists` / `waitFor` to gate optional behaviour
// instead of catching.
class AppDriver
{
public:
    explicit AppDriver(std::string rpcUrl, AppDriverOptions options = {});

    const std::string& rpcUrl() const;

    // Underlying RPC call — useful for hitting application commands
    // (the ones the app registers itself), not just test.* helpers.
    Miro::JSON invoke(const std::string& command,
                      const Miro::JSON& payload = {}) const;

    template <typename Resp, typename Req>
    Resp invoke(const std::string& command, const Req& req) const
    {
        auto json = client.invokeRaw(command, Miro::toJSON(req));
        auto out = Resp {};
        Miro::fromJSON(out, json);
        return out;
    }

    template <typename Resp>
    Resp invoke(const std::string& command) const
    {
        auto json = client.invokeRaw(command, Miro::JSON {Miro::Json::Object {}});
        auto out = Resp {};
        Miro::fromJSON(out, json);
        return out;
    }

    bool click(const std::string& selector, CallOptions opts = {}) const;
    bool fill(const std::string& selector, const std::string& value,
              CallOptions opts = {}) const;
    bool press(const std::string& selector, const std::string& key,
               CallOptions opts = {}) const;
    bool submit(const std::string& selector, CallOptions opts = {}) const;
    std::string text(const std::string& selector, CallOptions opts = {}) const;
    std::optional<std::string> attr(const std::string& selector,
                                    const std::string& name,
                                    CallOptions opts = {}) const;
    bool exists(const std::string& selector, CallOptions opts = {}) const;
    int count(const std::string& selector, CallOptions opts = {}) const;
    bool waitFor(const std::string& selector, CallOptions opts = {}) const;

    // Evaluates an arbitrary JS expression in the page. The
    // expression is wrapped in a function body so it can reference
    // window / document / page globals. Result must be JSON-
    // serialisable.
    Miro::JSON evaluate(const std::string& expression,
                        CallOptions opts = {}) const;

    template <typename T>
    T evaluate(const std::string& expression, CallOptions opts = {}) const
    {
        auto result = evaluate(expression, opts);
        auto out = T {};
        Miro::fromJSON(out, result);
        return out;
    }

    // Returns the serialised outerHTML of `selector`, or the full
    // document when selector is empty.
    std::string dom(std::string_view selector = {}, CallOptions opts = {}) const;

    // Captures a PNG of the visible WebView area. Returns the
    // decoded bytes; also writes to `options.path` when set.
    ScreenshotResult screenshot(const ScreenshotOptions& options = {}) const;

    // Captures DOM HTML + screenshot under one label, written into
    // <snapshotDir>/<name>.html and <name>.png.
    SnapshotResult snapshot(const std::string& name,
                            const SnapshotOptions& options = {}) const;

    // Runs `action`, then `snapshot(name)`, then returns the action's
    // result. The snapshot is taken even if `action` throws, so a
    // failing step still leaves an audit trail on disk.
    template <typename Fn>
    auto withSnapshot(const std::string& name, Fn&& action,
                      SnapshotOptions options = {}) const
    {
        struct SnapshotOnExit
        {
            const AppDriver& driver;
            const std::string& name;
            const SnapshotOptions& options;

            ~SnapshotOnExit()
            {
                try
                {
                    driver.snapshot(name, options);
                }
                catch (...)
                {
                    // Swallow — we don't want a snapshot failure to
                    // mask the original test failure.
                }
            }
        };

        auto guard = SnapshotOnExit {*this, name, options};
        return action();
    }

private:
    Miro::JSON runCommand(const std::string& command, Miro::JSON payload,
                          const CallOptions& opts) const;

    HTTP::Rpc::Client client;
    std::string rpcUrlValue;
    std::optional<int> defaultTimeoutMs;
    std::string snapshotDir;
};

} // namespace eacp::WebView::Test
