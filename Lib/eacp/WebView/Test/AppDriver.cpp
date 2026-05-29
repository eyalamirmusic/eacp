#include "AppDriver.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/WebView/WebView.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace eacp::WebView::Test
{

namespace
{

constexpr auto defaultTimeoutMs = 5000;
constexpr auto waitForPollMs = 50;
constexpr auto defaultSnapshotSubdir = "test-results/snapshots";

std::string jsStringLiteral(std::string_view value)
{
    auto out = std::string {"\""};
    out.reserve(value.size() + 2);

    for (auto c: value)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '<': out += "\\u003c"; break;
            case '>': out += "\\u003e"; break;
            case '&': out += "\\u0026"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += c;
                }
                break;
        }
    }
    out += '"';
    return out;
}

// Wraps an arbitrary JS expression so the callback always gets a
// JSON-encoded {value: ...}. JSON.stringify of undefined returns
// undefined (not a string), which evaluateJavaScript would surface
// as an empty result — the explicit object wrapper avoids that
// ambiguity.
std::string wrapExpr(const std::string& expression)
{
    return "JSON.stringify({value:(" + expression + ")})";
}

std::string resolveSnapshotDir(std::string fromOptions)
{
    if (!fromOptions.empty())
        return fromOptions;

    auto cwd = std::filesystem::current_path();
    return (cwd / defaultSnapshotSubdir).string();
}

std::string sanitizeSnapshotName(const std::string& name)
{
    auto out = std::string {};
    out.reserve(name.size());
    for (auto c: name)
    {
        auto ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                  || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '/'
                  || c == '-';
        out.push_back(ok ? c : '_');
    }
    return out;
}

void writeBinary(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes)
{
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());

    auto file = std::ofstream {path, std::ios::binary | std::ios::trunc};
    if (!file)
        throw std::runtime_error("AppDriver: failed to open '" + path.string()
                                 + "' for writing");
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file)
        throw std::runtime_error("AppDriver: short write to '" + path.string()
                                 + "'");
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());

    auto file = std::ofstream {path, std::ios::trunc};
    if (!file)
        throw std::runtime_error("AppDriver: failed to open '" + path.string()
                                 + "' for writing");
    file << text;
    if (!file)
        throw std::runtime_error("AppDriver: short write to '" + path.string()
                                 + "'");
}

bool asBool(const Miro::JSON& v)
{
    return v.isBool() && v.asBool();
}

std::string asString(const Miro::JSON& v)
{
    if (v.isString())
        return v.asString();
    if (v.isNull())
        return {};
    throw std::runtime_error("AppDriver: expected string result");
}

int asInt(const Miro::JSON& v)
{
    if (v.isNumber())
        return static_cast<int>(v.asNumber());
    throw std::runtime_error("AppDriver: expected numeric result");
}

} // namespace

AppDriver::AppDriver(Graphics::WebView& webViewToUse,
                     Miro::Bridge& bridgeToUse, AppDriverOptions options)
    : webView(webViewToUse)
    , bridge(bridgeToUse)
    , defaultTimeoutMs(options.defaultTimeoutMs)
    , snapshotDir(resolveSnapshotDir(std::move(options.snapshotDir)))
    , firstNavigation(firstNavigationPromise.get())
{
    // evaluateJavaScript before the first navigation finishes
    // sometimes fails — the JS context isn't fully set up yet. Latch
    // on the first didFinishNavigation callback so command methods
    // can wait for the page to be ready (either by awaiting the
    // promise from a coroutine, or by pumping it via waitFor in the
    // sync path).
    //
    // Chain through whatever was already installed so users aren't
    // surprised by their own handler getting clobbered.
    previousFinishedHandler = webView.onNavigationFinished;
    webView.onNavigationFinished =
        [this, previous = previousFinishedHandler](const std::string& url)
    {
        if (previous)
            previous(url);

        if (firstNavigationFired)
            return;

        firstNavigationFired = true;
        firstNavigationPromise.resolve();
    };

    // Latch on failure too, so a broken scheme / 404 / etc. unblocks
    // the wait immediately with the actual WebView error instead of
    // hanging until the timeout. The reject() carries the error string
    // straight through to the AsyncError that waitForFirstNavigation /
    // any co_await on the promise will surface.
    previousFailedHandler = webView.onNavigationFailed;
    webView.onNavigationFailed =
        [this, previous = previousFailedHandler](const std::string& error)
    {
        if (previous)
            previous(error);

        if (firstNavigationFired)
            return;

        firstNavigationFired = true;
        firstNavigationPromise.reject(error);
    };
}

AppDriver::~AppDriver()
{
    // Restore the user's handlers so a longer-lived WebView doesn't
    // keep firing into this dead driver.
    webView.onNavigationFinished = std::move(previousFinishedHandler);
    webView.onNavigationFailed = std::move(previousFailedHandler);
}

int AppDriver::effectiveTimeoutMs(const CallOptions& opts) const
{
    if (opts.timeoutMs)
        return *opts.timeoutMs;
    if (defaultTimeoutMs)
        return *defaultTimeoutMs;
    return defaultTimeoutMs ? *defaultTimeoutMs : Test::defaultTimeoutMs;
}

Threads::Async<void>
AppDriver::waitForFirstNavigationAsync(const CallOptions&)
{
    return firstNavigation;
}

void AppDriver::waitForFirstNavigation(const CallOptions& opts)
{
    auto timeout = std::chrono::milliseconds {effectiveTimeoutMs(opts)};
    try
    {
        firstNavigation.waitFor(timeout);
    }
    catch (const Threads::AsyncError& e)
    {
        // If the latch has fired by the time waitFor throws, the
        // promise was reject()'d (by onNavigationFailed) — surface the
        // actual WebView2 error. Otherwise we hit Async::waitFor's own
        // "timed out" path; report it as a load timeout.
        if (firstNavigationFired)
            throw std::runtime_error("AppDriver: navigation failed: "
                                     + std::string {e.what()});

        throw std::runtime_error("AppDriver: page did not finish loading "
                                 "within "
                                 + std::to_string(timeout.count()) + "ms");
    }
}

namespace
{

struct SnapshotState
{
    std::vector<std::uint8_t> bytes;
    std::string error;
    bool done = false;
};

Miro::JSON unwrapJsResult(const std::string& raw)
{
    if (raw.empty())
        return Miro::JSON {};

    auto parsed = Miro::JSON {};
    try
    {
        parsed = Miro::Json::parse(raw);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string {"AppDriver: failed to parse JS "
                                              "result: "}
                                 + e.what() + " (raw: " + raw + ")");
    }

    if (!parsed.isObject())
        return parsed;

    auto& obj = parsed.asObject();
    auto it = obj.find("value");
    return it != obj.end() ? it->second : Miro::JSON {};
}

// Outer waitFor on a sync wrapper needs to outlive the per-call
// timeout so the coroutine's own deadline check throws the
// task-specific message ("waitFor timed out for selector …",
// "JS evaluation timed out …") before the generic
// "Async::waitFor timed out" from the wrapper.
constexpr auto syncTimeoutBufferMs = 1000;

std::chrono::milliseconds syncOuterTimeout(int innerTimeoutMs)
{
    return std::chrono::milliseconds {innerTimeoutMs + syncTimeoutBufferMs};
}

} // namespace

Threads::Async<Miro::JSON>
AppDriver::runJsAsync(const std::string& expression, const CallOptions& opts)
{
    co_await waitForFirstNavigationAsync(opts);

    auto raw = std::string {};
    try
    {
        raw = co_await webView.callJS(wrapExpr(expression));
    }
    catch (const Threads::AsyncError& e)
    {
        throw std::runtime_error(std::string {"AppDriver JS error: "}
                                 + e.what());
    }

    co_return unwrapJsResult(raw);
}

Miro::JSON AppDriver::runJs(const std::string& expression,
                            const CallOptions& opts)
{
    auto timeoutMs = effectiveTimeoutMs(opts);
    return runJsAsync(expression, opts).waitFor(syncOuterTimeout(timeoutMs));
}

std::vector<std::uint8_t>
AppDriver::runSnapshotBytes(const CallOptions& opts)
{
    waitForFirstNavigation(opts);

    auto timeout = std::chrono::milliseconds {effectiveTimeoutMs(opts)};
    auto state = SnapshotState {};

    webView.takeSnapshot(
        [&state](std::vector<std::uint8_t> bytes, const std::string& error)
        {
            state.bytes = std::move(bytes);
            state.error = error;
            state.done = true;
            Threads::stopEventLoop();
        });

    if (!state.done)
    {
        if (!Threads::runEventLoopFor(timeout) && !state.done)
            throw std::runtime_error("AppDriver: screenshot timed out after "
                                     + std::to_string(timeout.count()) + "ms");
    }

    if (!state.error.empty())
        throw std::runtime_error("AppDriver: " + state.error);

    return std::move(state.bytes);
}

Miro::JSON AppDriver::invoke(const std::string& command,
                             const Miro::JSON& payload)
{
    return bridge.dispatch(command, payload);
}

Threads::Async<bool>
AppDriver::clickAsync(const std::string& selector, CallOptions opts)
{
    auto result = co_await runJsAsync(
        "window.__test.click(" + jsStringLiteral(selector) + ")", opts);
    co_return asBool(result);
}

bool AppDriver::click(const std::string& selector, CallOptions opts)
{
    return clickAsync(selector, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<bool> AppDriver::fillAsync(const std::string& selector,
                                          const std::string& value,
                                          CallOptions opts)
{
    auto result = co_await runJsAsync(
        "window.__test.fill(" + jsStringLiteral(selector) + ","
            + jsStringLiteral(value) + ")",
        opts);
    co_return asBool(result);
}

bool AppDriver::fill(const std::string& selector, const std::string& value,
                     CallOptions opts)
{
    return fillAsync(selector, value, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<bool> AppDriver::pressAsync(const std::string& selector,
                                           const std::string& key,
                                           CallOptions opts)
{
    auto result = co_await runJsAsync(
        "window.__test.press(" + jsStringLiteral(selector) + ","
            + jsStringLiteral(key) + ")",
        opts);
    co_return asBool(result);
}

bool AppDriver::press(const std::string& selector, const std::string& key,
                      CallOptions opts)
{
    return pressAsync(selector, key, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<bool>
AppDriver::submitAsync(const std::string& selector, CallOptions opts)
{
    auto result = co_await runJsAsync(
        "window.__test.submit(" + jsStringLiteral(selector) + ")", opts);
    co_return asBool(result);
}

bool AppDriver::submit(const std::string& selector, CallOptions opts)
{
    return submitAsync(selector, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<std::string>
AppDriver::textAsync(const std::string& selector, CallOptions opts)
{
    auto result = co_await runJsAsync(
        "window.__test.text(" + jsStringLiteral(selector) + ")", opts);
    co_return asString(result);
}

std::string AppDriver::text(const std::string& selector, CallOptions opts)
{
    return textAsync(selector, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<std::optional<std::string>>
AppDriver::attrAsync(const std::string& selector, const std::string& name,
                     CallOptions opts)
{
    auto result = co_await runJsAsync(
        "window.__test.attr(" + jsStringLiteral(selector) + ","
            + jsStringLiteral(name) + ")",
        opts);
    if (result.isNull())
        co_return std::nullopt;
    co_return asString(result);
}

std::optional<std::string> AppDriver::attr(const std::string& selector,
                                           const std::string& name,
                                           CallOptions opts)
{
    return attrAsync(selector, name, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<bool>
AppDriver::existsAsync(const std::string& selector, CallOptions opts)
{
    auto result = co_await runJsAsync(
        "window.__test.exists(" + jsStringLiteral(selector) + ")", opts);
    co_return asBool(result);
}

bool AppDriver::exists(const std::string& selector, CallOptions opts)
{
    return existsAsync(selector, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<int>
AppDriver::countAsync(const std::string& selector, CallOptions opts)
{
    auto result = co_await runJsAsync(
        "window.__test.count(" + jsStringLiteral(selector) + ")", opts);
    co_return asInt(result);
}

int AppDriver::count(const std::string& selector, CallOptions opts)
{
    return countAsync(selector, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<bool>
AppDriver::waitForAsync(const std::string& selector, CallOptions opts)
{
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds {effectiveTimeoutMs(opts)};

    while (true)
    {
        auto result = co_await runJsAsync(
            "window.__test.exists(" + jsStringLiteral(selector) + ")", {});
        if (asBool(result))
            co_return true;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("AppDriver: waitFor timed out for "
                                     "selector: "
                                     + selector);
        co_await Threads::delay(std::chrono::milliseconds {waitForPollMs});
    }
}

bool AppDriver::waitFor(const std::string& selector, CallOptions opts)
{
    return waitForAsync(selector, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<Miro::JSON>
AppDriver::evaluateAsync(const std::string& expression, CallOptions opts)
{
    return runJsAsync(
        "window.__test.evaluate(" + jsStringLiteral(expression) + ")", opts);
}

Miro::JSON AppDriver::evaluate(const std::string& expression, CallOptions opts)
{
    return evaluateAsync(expression, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

Threads::Async<std::string>
AppDriver::domAsync(std::string_view selector, CallOptions opts)
{
    auto arg = selector.empty() ? std::string {"null"}
                                : jsStringLiteral(selector);
    auto result = co_await runJsAsync("window.__test.dom(" + arg + ")", opts);
    co_return asString(result);
}

std::string AppDriver::dom(std::string_view selector, CallOptions opts)
{
    return domAsync(selector, opts)
        .waitFor(syncOuterTimeout(effectiveTimeoutMs(opts)));
}

ScreenshotResult AppDriver::screenshot(const ScreenshotOptions& options)
{
    auto callOpts = CallOptions {.timeoutMs = options.timeoutMs};
    auto bytes = runSnapshotBytes(callOpts);

    auto result = ScreenshotResult {};
    result.png = std::move(bytes);

    if (!options.path.empty())
    {
        writeBinary(options.path, result.png);
        result.path = options.path;
    }

    return result;
}

SnapshotResult AppDriver::snapshot(const std::string& name,
                                   const SnapshotOptions& options)
{
    auto baseName = sanitizeSnapshotName(name);
    auto baseDir = options.dir.empty() ? snapshotDir : options.dir;

    auto htmlPath = std::filesystem::path {baseDir} / (baseName + ".html");
    auto pngPath = std::filesystem::path {baseDir} / (baseName + ".png");

    auto callOpts = CallOptions {.timeoutMs = options.timeoutMs};

    auto html = dom(options.selector, callOpts);
    auto shot = screenshot({.timeoutMs = options.timeoutMs,
                            .path = pngPath.string()});

    writeText(htmlPath, html);

    return SnapshotResult {.name = name,
                           .dom = std::move(html),
                           .png = std::move(shot.png),
                           .domPath = htmlPath.string(),
                           .screenshotPath = pngPath.string()};
}

} // namespace eacp::WebView::Test
