#pragma once

#include <eacp/Core/Threads/Async.h>
#include <Miro/Miro.h>

namespace eacp::Graphics
{
class WebView;
}

namespace eacp::WebView::Test
{

struct CallOptions
{
    // Per-call timeout override. Empty -> driver default; if that's
    // also empty, defaultTimeoutMs constant below.
    std::optional<int> timeoutMs;
};

struct AppDriverOptions
{
    // Default per-command timeout for this driver. Empty -> the
    // built-in 5s applies.
    std::optional<int> defaultTimeoutMs;

    // Directory snapshot() writes <name>.html + <name>.png into.
    // Empty -> "<cwd>/test-results/snapshots". Created lazily.
    std::string snapshotDir;
};

struct ScreenshotOptions
{
    std::optional<int> timeoutMs;

    // Decode + write the PNG to this path. Empty -> do not write.
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

// In-process driver for a WebView app. Lives on the main thread
// (same thread the WebView and its bridge run on). Each operation
// schedules work on the WebView, then pumps the event loop via
// runEventLoopFor() until the callback completes. No threading,
// no HTTP — everything is direct calls.
//
// Methods throw std::runtime_error on JS exceptions, timeouts, or
// missing selectors. Use exists()/waitFor() to gate optional
// behaviour instead of catching.
class AppDriver
{
public:
    AppDriver(Graphics::WebView& webViewToUse,
              Miro::Bridge& bridgeToUse,
              AppDriverOptions options = {});
    ~AppDriver();

    AppDriver(const AppDriver&) = delete;
    AppDriver& operator=(const AppDriver&) = delete;
    AppDriver(AppDriver&&) = delete;
    AppDriver& operator=(AppDriver&&) = delete;

    // Application bridge commands — direct dispatch, no JS hop.
    Miro::JSON invoke(const std::string& command, const Miro::JSON& payload = {});

    template <typename Resp, typename Req>
    Resp invoke(const std::string& command, const Req& req)
    {
        auto json = invoke(command, Miro::toJSON(req));
        auto out = Resp {};
        Miro::fromJSON(out, json);
        return out;
    }

    template <typename Resp>
    Resp invoke(const std::string& command)
    {
        auto json = invoke(command, Miro::JSON {Miro::Json::Object {}});
        auto out = Resp {};
        Miro::fromJSON(out, json);
        return out;
    }

    bool click(const std::string& selector, CallOptions opts = {});
    bool fill(const std::string& selector,
              const std::string& value,
              CallOptions opts = {});
    bool press(const std::string& selector,
               const std::string& key,
               CallOptions opts = {});
    bool submit(const std::string& selector, CallOptions opts = {});
    std::string text(const std::string& selector, CallOptions opts = {});
    std::optional<std::string> attr(const std::string& selector,
                                    const std::string& name,
                                    CallOptions opts = {});
    bool exists(const std::string& selector, CallOptions opts = {});
    int count(const std::string& selector, CallOptions opts = {});
    bool waitFor(const std::string& selector, CallOptions opts = {});

    // Evaluates an arbitrary JS expression. Wrapped so the result is
    // round-trip JSON-shaped; the unwrapped value is returned.
    Miro::JSON evaluate(const std::string& expression, CallOptions opts = {});

    template <typename T>
    T evaluate(const std::string& expression, CallOptions opts = {})
    {
        auto result = evaluate(expression, opts);
        auto out = T {};
        Miro::fromJSON(out, result);
        return out;
    }

    std::string dom(std::string_view selector = {}, CallOptions opts = {});

    // Async siblings of the above. Each returns an Async that resolves
    // on the main thread; use with co_await inside a coroutine test
    // body. Rejection surfaces as an AsyncError (same message the
    // sync variant would throw as std::runtime_error).
    Threads::Async<bool> clickAsync(const std::string& selector,
                                    CallOptions opts = {});
    Threads::Async<bool> fillAsync(const std::string& selector,
                                   const std::string& value,
                                   CallOptions opts = {});
    Threads::Async<bool> pressAsync(const std::string& selector,
                                    const std::string& key,
                                    CallOptions opts = {});
    Threads::Async<bool> submitAsync(const std::string& selector,
                                     CallOptions opts = {});
    Threads::Async<std::string> textAsync(const std::string& selector,
                                          CallOptions opts = {});
    Threads::Async<std::optional<std::string>> attrAsync(const std::string& selector,
                                                         const std::string& name,
                                                         CallOptions opts = {});
    Threads::Async<bool> existsAsync(const std::string& selector,
                                     CallOptions opts = {});
    Threads::Async<int> countAsync(const std::string& selector,
                                   CallOptions opts = {});
    Threads::Async<bool> waitForAsync(const std::string& selector,
                                      CallOptions opts = {});
    Threads::Async<Miro::JSON> evaluateAsync(const std::string& expression,
                                             CallOptions opts = {});
    Threads::Async<std::string> domAsync(std::string_view selector = {},
                                         CallOptions opts = {});

    ScreenshotResult screenshot(const ScreenshotOptions& options = {});
    SnapshotResult snapshot(const std::string& name,
                            const SnapshotOptions& options = {});

    template <typename Fn>
    auto withSnapshot(const std::string& name,
                      Fn&& action,
                      const SnapshotOptions& options = {})
    {
        struct SnapshotOnExit
        {
            AppDriver& driver;
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
                    // Don't mask the original failure.
                }
            }
        };

        auto guard = SnapshotOnExit {*this, name, options};
        return action();
    }

private:
    Threads::Async<Miro::JSON> runJsAsync(const std::string& expression,
                                          const CallOptions& opts);
    Miro::JSON runJs(const std::string& expression, const CallOptions& opts);
    std::vector<std::uint8_t> runSnapshotBytes(const CallOptions& opts);
    Threads::Async<> waitForFirstNavigationAsync(const CallOptions& opts);
    void waitForFirstNavigation(const CallOptions& opts);
    int effectiveTimeoutMs(const CallOptions& opts) const;

    eacp::Graphics::WebView& webView;
    Miro::Bridge& bridge;
    std::optional<int> defaultTimeoutMs;
    std::string snapshotDir;

    Threads::AsyncPromise<> firstNavigationPromise;
    Threads::Async<> firstNavigation;
    bool firstNavigationFired = false;
    std::function<void(const std::string&)> previousFinishedHandler;
    std::function<void(const std::string&)> previousFailedHandler;
};

} // namespace eacp::WebView::Test
