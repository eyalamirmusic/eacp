#pragma once

#include "../Common.h"

#include "DomNode.h"

namespace eacp::Graphics
{
class WebView;
}

namespace eacp::WebView::Test
{

struct CallOptions
{
    // Empty -> the driver default, else the built-in 5s.
    std::optional<int> timeoutMs;
};

struct AppDriverOptions
{
    // Empty -> the built-in 5s.
    std::optional<int> defaultTimeoutMs;

    // Empty -> "<cwd>/test-results/snapshots". Created lazily.
    std::string snapshotDir;
};

struct ScreenshotOptions
{
    std::optional<int> timeoutMs;

    // Format inferred from the extension (.png/.jpg/.jpeg). Empty -> no write.
    std::string path;
};

struct ScreenshotResult
{
    Graphics::Image image;

    // Set only when ScreenshotOptions::path was non-empty.
    std::string path;
};

struct SnapshotOptions
{
    std::optional<int> timeoutMs;

    // Overrides the driver's configured snapshot directory.
    std::string dir;

    // Empty -> the whole document.
    std::string selector;
};

struct SnapshotResult
{
    std::string name;
    std::string dom;
    Graphics::Image image;
    std::string domPath;
    std::string screenshotPath;
};

// In-process driver for a WebView app; main-thread only. The blocking
// methods pump the event loop until their callback completes, and throw
// std::runtime_error on JS exceptions, timeouts, or missing selectors.
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

    // Dispatches straight to the bridge, without a JS hop.
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

    // Unlike waitFor(), does not return early on a list that is still growing.
    bool waitForCount(const std::string& selector, int count, CallOptions opts = {});

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

    // query() throws when nothing matches; tryQuery() returns nullopt.
    DomNode query(const std::string& selector, CallOptions opts = {});
    std::optional<DomNode> tryQuery(const std::string& selector,
                                    CallOptions opts = {});
    Vector<DomNode> queryAll(const std::string& selector, CallOptions opts = {});

    // Resolve on the main thread; failures reject with AsyncError instead
    // of throwing.
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
    Threads::Async<bool> waitForCountAsync(const std::string& selector,
                                           int count,
                                           CallOptions opts = {});
    Threads::Async<Miro::JSON> evaluateAsync(const std::string& expression,
                                             CallOptions opts = {});
    Threads::Async<std::string> domAsync(std::string_view selector = {},
                                         CallOptions opts = {});
    Threads::Async<DomNode> queryAsync(const std::string& selector,
                                       CallOptions opts = {});
    Threads::Async<std::optional<DomNode>> tryQueryAsync(const std::string& selector,
                                                         CallOptions opts = {});
    Threads::Async<Vector<DomNode>> queryAllAsync(const std::string& selector,
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

            AppDriver& driver;
            const std::string& name;
            const SnapshotOptions& options;
        };

        auto guard = SnapshotOnExit {*this, name, options};
        return action();
    }

private:
    Threads::Async<Miro::JSON> runJsAsync(const std::string& expression,
                                          const CallOptions& opts);
    Miro::JSON runJs(const std::string& expression, const CallOptions& opts);
    Vector<std::uint8_t> runSnapshotBytes(const CallOptions& opts);
    Threads::Async<> waitForFirstNavigationAsync(const CallOptions& opts);
    void waitForFirstNavigation(const CallOptions& opts);
    int effectiveTimeoutMs(const CallOptions& opts) const;
    Time::MS syncOuterTimeout(int innerTimeoutMs) const;

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
