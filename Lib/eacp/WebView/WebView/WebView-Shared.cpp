#include "WebView.h"
#include <algorithm>
#include <utility>

#include "DevServerProbe.h"
#include "JsStringLiteral.h"
#include "StreamingRange.h"
#include "WebViewDetail.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Singleton.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Graphics/Image/Image.h>

namespace eacp::Graphics
{
WebView::SnapshotCallback
    detail::withSnapshotDeadline(WebView::SnapshotCallback callback,
                                 Time::MS deadline)
{
    auto answered = std::make_shared<bool>(false);

    auto once = [answered, callback = std::move(callback)](Bytes pngBytes,
                                                           const std::string& error)
    {
        if (std::exchange(*answered, true))
            return;

        callback(std::move(pngBytes), error);
    };

    // The deadline rides the run loop rather than a thread of its own, and a
    // platform answer that beat it simply makes this a no-op when it lands.
    Threads::callAfter(deadline, [once] { once({}, "snapshot timed out"); });

    return once;
}

void WebView::captureAsyncContent(float, std::function<void(Image)> done)
{
    takeSnapshot(
        [done = std::move(done)](Bytes pngBytes, const std::string& error)
        {
            if (!error.empty() || pngBytes.empty())
            {
                done({});
                return;
            }

            done(Image::decode(pngBytes));
        });
}

// Matches on a lowercased copy so MyClip.WAV maps the same as .wav.
std::string mimeForPath(std::string_view path)
{
    auto lower = Strings::toLower(path);

    auto endsWith = [&](std::string_view ext) { return lower.ends_with(ext); };

    if (endsWith(".html"))
        return "text/html; charset=utf-8";
    if (endsWith(".js") || endsWith(".mjs") || endsWith(".cjs"))
        return "application/javascript; charset=utf-8";
    if (endsWith(".css"))
        return "text/css; charset=utf-8";
    if (endsWith(".json") || endsWith(".map"))
        return "application/json; charset=utf-8";
    if (endsWith(".wasm"))
        return "application/wasm";

    if (endsWith(".svg"))
        return "image/svg+xml";
    if (endsWith(".png"))
        return "image/png";
    if (endsWith(".jpg") || endsWith(".jpeg"))
        return "image/jpeg";
    if (endsWith(".gif"))
        return "image/gif";
    if (endsWith(".webp"))
        return "image/webp";

    if (endsWith(".woff2"))
        return "font/woff2";

    if (endsWith(".mp3"))
        return "audio/mpeg";
    if (endsWith(".wav"))
        return "audio/wav";
    if (endsWith(".aif") || endsWith(".aiff"))
        return "audio/aiff";
    if (endsWith(".flac"))
        return "audio/flac";
    if (endsWith(".m4a") || endsWith(".mp4"))
        return "audio/mp4";
    if (endsWith(".aac"))
        return "audio/aac";
    if (endsWith(".ogg") || endsWith(".opus"))
        return "audio/ogg";

    return "application/octet-stream";
}

// WebKit hands custom-scheme handlers the full URL including any fragment
// (it only strips fragments for standard schemes like http). A hash-routed
// SPA loads e.g. app://local/index.html#/route — the fragment is stripped so
// the resource key stays "index.html", and the query string the same way.
std::string pathFromURL(std::string_view url, std::string_view indexFile)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string_view::npos)
        return {};

    auto afterHost = url.find('/', schemeEnd + 3);

    if (afterHost == std::string_view::npos)
        return std::string(indexFile);

    auto path = url.substr(afterHost + 1);

    auto fragment = path.find('#');

    if (fragment != std::string_view::npos)
        path = path.substr(0, fragment);

    auto query = path.find('?');

    if (query != std::string_view::npos)
        path = path.substr(0, query);

    return path.empty() ? std::string(indexFile) : std::string(path);
}

namespace
{
std::optional<RangeSize> parseRangeSize(std::string_view text)
{
    if (text.empty())
        return std::nullopt;

    auto value = RangeSize {0};

    for (auto c: text)
    {
        if (c < '0' || c > '9')
            return std::nullopt;

        value = value * 10 + static_cast<RangeSize>(c - '0');
    }

    return value;
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string percentDecode(std::string_view encoded)
{
    auto out = std::string {};
    out.reserve(encoded.size());

    for (auto i = 0; i < (int) encoded.size(); ++i)
    {
        if (encoded[i] == '%' && i + 2 < (int) encoded.size())
        {
            auto hi = hexDigit(encoded[i + 1]);
            auto lo = hexDigit(encoded[i + 2]);

            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }

        out.push_back(encoded[i]);
    }

    return out;
}
} // namespace

// Serves a single range only: a comma marks a multi-range request, which
// falls back to the full resource. The suffix form `bytes=-N` selects the
// last N bytes.
ResolvedRange resolveRangeHeader(std::string_view headerValue, RangeSize size)
{
    auto full = ResolvedRange {RangeRequest::Full, ByteRange {0, size}};
    auto unsatisfiable = ResolvedRange {RangeRequest::Unsatisfiable, ByteRange {}};

    constexpr auto prefix = std::string_view {"bytes="};

    if (!headerValue.starts_with(prefix))
        return full;

    auto spec = headerValue.substr(prefix.size());

    auto dash = spec.find('-');

    if (spec.find(',') != std::string_view::npos || dash == std::string_view::npos)
        return full;

    if (size == 0)
        return unsatisfiable;

    auto firstText = spec.substr(0, dash);
    auto lastText = spec.substr(dash + 1);

    if (firstText.empty())
    {
        auto suffix = parseRangeSize(lastText);

        if (!suffix)
            return full;

        if (*suffix == 0)
            return unsatisfiable;

        auto length = std::min(*suffix, size);
        return {RangeRequest::Partial, ByteRange {size - length, length}};
    }

    auto first = parseRangeSize(firstText);

    if (!first)
        return full;

    if (*first >= size)
        return unsatisfiable;

    auto lastIndex = size - 1;

    if (!lastText.empty())
    {
        auto last = parseRangeSize(lastText);

        if (!last)
            return full;

        lastIndex = std::min(*last, size - 1);

        if (lastIndex < *first)
            return unsatisfiable;
    }

    return {RangeRequest::Partial, ByteRange {*first, lastIndex - *first + 1}};
}

std::string contentRangeValue(const ByteRange& served, RangeSize size)
{
    return "bytes " + std::to_string(served.start) + "-"
           + std::to_string(served.end() - 1) + "/" + std::to_string(size);
}

// WebView2 enforces cross-origin rules on our own custom scheme, so fetch()
// needs an explicit allow-origin header; added here so both backends agree.
StreamingResponsePlan planStreamingResponse(std::string_view rangeHeader,
                                            const StreamingResource& resource)
{
    auto resolved = resolveRangeHeader(rangeHeader, resource.size);

    auto plan = StreamingResponsePlan {};
    plan.served = resolved.served;
    plan.statusCode = resource.statusCode;

    plan.headers.add({"Content-Type", resource.mimeType});
    plan.headers.add({"Access-Control-Allow-Origin", "*"});
    plan.headers.add({"Accept-Ranges", "bytes"});

    if (resolved.kind == RangeRequest::Unsatisfiable)
    {
        plan.statusCode = 416;
        plan.headers.add(
            {"Content-Range", "bytes */" + std::to_string(resource.size)});
        plan.hasBody = false;
    }
    else
    {
        if (resolved.kind == RangeRequest::Partial)
        {
            plan.statusCode = 206;
            plan.headers.add(
                {"Content-Range", contentRangeValue(plan.served, resource.size)});
        }

        plan.headers.add({"Content-Length", std::to_string(plan.served.length)});
        plan.hasBody = !plan.served.empty();
    }

    return plan;
}

// A Windows drive path arrives as "/C:/dir/file"; the leading slash is
// dropped so it parses as the native "C:/dir/file". POSIX paths ("/var/...")
// have no drive letter and are left untouched.
std::string fileURLToPath(std::string_view url)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string_view::npos)
        return {};

    auto rest = url.substr(schemeEnd + 3);
    auto cut = rest.find_first_of("?#");

    if (cut != std::string_view::npos)
        rest = rest.substr(0, cut);

    auto slash = rest.find('/');

    if (slash == std::string_view::npos)
        return {};

    auto decoded = percentDecode(rest.substr(slash));

    if (decoded.size() >= 3 && decoded[0] == '/'
        && std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':')
        decoded.erase(0, 1);

    return decoded;
}

FileProvider fromResEmbed(std::string category)
{
    return [category = std::move(category)](
               std::string_view path) -> std::optional<ByteView>
    {
        auto view = ResEmbed::get(std::string(path), category);

        if (!view)
            return std::nullopt;

        return ByteView {view.data(), view.getSize()};
    };
}

// The file is kept open behind a shared_ptr so the reader can pull chunks
// across many scheme-task callbacks, then closes when the last reader drops.
// File::isUnder canonicalises, so a raw path is fine.
StreamingProvider
    fileStreamProvider(Vector<std::string> roots,
                       std::function<std::string(std::string_view path)> mimeForFile)
{
    return [roots = std::move(roots), mimeForFile = std::move(mimeForFile)](
               std::string_view url) -> std::optional<StreamingResource>
    {
        auto pathStr = fileURLToPath(url);

        if (pathStr.empty())
            return std::nullopt;

        auto file = std::make_shared<eacp::File>(pathStr);

        auto allowed =
            roots.empty()
            || std::any_of(roots.begin(),
                           roots.end(),
                           [&](const auto& root) { return file->isUnder(root); });

        if (!allowed || !file->isRegularFile() || !file->openForRead())
            return std::nullopt;

        auto response = StreamingResource {};
        response.mimeType =
            mimeForFile ? mimeForFile(pathStr) : mimeForPath(pathStr);
        response.size = file->size();
        response.read = [file](RangeSize offset, ByteSpan out)
        { return (int) file->read(offset, out); };
        return response;
    };
}

namespace
{
ResourceProvider makeResourceProviderFromFiles(FileProvider provider,
                                               std::string indexFile)
{
    return [provider = std::move(provider), indexFile = std::move(indexFile)](
               std::string_view url) -> std::optional<ResourceResponse>
    {
        auto path = pathFromURL(url, indexFile);
        auto bytes = provider ? provider(path) : std::nullopt;

        if (!bytes)
            return std::nullopt;

        ResourceResponse response;
        response.mimeType = mimeForPath(path);
        response.data.assign(bytes->begin(), bytes->end());
        return response;
    };
}

void registerEmbeddedScheme(WebView::Options& options)
{
    options.schemes[options.embedded.scheme] = makeResourceProviderFromFiles(
        options.embedded.provider, options.embedded.indexFile);
}

bool shouldUseDevServer(const WebView::Options::Embedded& embedded)
{
    if (!embedded.enabled || !embedded.preferDevServer
        || embedded.devServerURL.empty())
        return false;

    return probeDevServer(embedded.devServerURL, embedded.devServerProbeTimeoutMs);
}
} // namespace

WebView::WebView()
    : WebView(Options {})
{
}

// Headless test harnesses install user scripts and navigation callbacks
// AFTER construction (TestApp wires the agent script and AppDriver hook once
// the WebView already exists). Loading inline would race those — the first
// navigation could fire before the agent is registered — so headless runs
// defer the initial load to the next runloop tick, letting the harness
// finish wiring before the load starts.
WebView::WebView(Options options)
{
    auto useDevServer = shouldUseDevServer(options.embedded);

    if (options.embedded.enabled && !useDevServer)
        registerEmbeddedScheme(options);

    auto embedded = options.embedded;
    initNative(std::move(options));

    if (!embedded.enabled || !embedded.autoLoad)
        return;

    auto url = useDevServer ? embedded.devServerURL
                            : embedded.scheme + "://" + embedded.host + "/"
                                  + embedded.indexFile;

    if (Apps::getAppEnvironment().headless)
    {
        auto weak = std::weak_ptr<Native> {impl};
        Threads::callAsync(
            [this, weak, url]
            {
                if (weak.lock())
                    loadURL(url);
            });
    }
    else
    {
        loadURL(url);
    }
}

// Injects window-drag.js + its message handler. Shared by both desktop
// backends, so it lives here once rather than in each initNative.
void WebView::installWindowDragSupport()
{
    auto shim = ResEmbed::get("window-drag.js", "EacpWebView");
    if (!shim)
        throw std::runtime_error(
            "eacp-webview: embedded window-drag.js resource not found");

    addUserScript(shim.toString(), true);
    addScriptMessageHandler("__eacpWindowDrag",
                            [this](const std::string&) { armWindowDrag(); });
}

// Injects window-controls.js + its message handler: any element marked
// `--eacp-window-button: minimize | maximize | close` becomes a working
// caption button, and pages key their chrome CSS off the data-eacp-platform /
// data-eacp-maximized attributes the shim keeps on <html>. Shared by both
// desktop backends, like installWindowDragSupport above.
void WebView::installWindowControlSupport()
{
    auto shim = ResEmbed::get("window-controls.js", "EacpWebView");
    if (!shim)
        throw std::runtime_error(
            "eacp-webview: embedded window-controls.js resource not found");

    auto platform = std::string {Platform::isWindows() ? "windows" : "mac"};
    addUserScript("window.__eacpPlatform = '" + platform + "';\n" + shim.toString(),
                  true);
    addScriptMessageHandler("__eacpWindowControl",
                            [this](const std::string& action)
                            { performWindowControl(action); });
}

// Redirects requestAnimationFrame onto a ~60 Hz timer at document-start, so an
// rAF loop keeps running while the view is off-screen (where the platform fires
// no animation frames) and each snapshot reflects live content. Gated on
// Options::driveOffscreenAnimation — see that flag. The script is tiny, so it is
// inlined rather than carried as an embedded resource.
void WebView::installOffscreenAnimationSupport()
{
    addUserScript(
        "(function () {\n"
        "  var frameMs = 16;\n"
        "  window.requestAnimationFrame = function (cb) {\n"
        "    return setTimeout(function () { cb(performance.now()); }, frameMs);\n"
        "  };\n"
        "  window.cancelAnimationFrame = function (id) { clearTimeout(id); };\n"
        "})();",
        true);
}

namespace detail
{
// Immortal because ~WebView unregisters here, and a WebView at namespace scope
// outlives a registry that first use constructed after it.
Vector<WebView*>& registeredWebViews()
{
    return Singleton::getImmortal<Vector<WebView*>>();
}

void registerWebView(WebView* view)
{
    registeredWebViews().add(view);
}

void unregisterWebView(WebView* view)
{
    registeredWebViews().removeAllMatches(view);
}
} // namespace detail

namespace
{
constexpr auto minZoomLevel = 0.25;
constexpr auto maxZoomLevel = 5.0;
constexpr auto zoomStep = 1.1;
} // namespace

double detail::clampZoom(double level)
{
    return std::clamp(level, minZoomLevel, maxZoomLevel);
}

void WebView::zoomIn()
{
    setZoom(getZoom() * zoomStep);
}

void WebView::zoomOut()
{
    setZoom(getZoom() / zoomStep);
}

void WebView::resetZoom()
{
    setZoom(1.0);
}

// On Windows, WebView2's ExecuteScript reports JS exceptions as a "null"
// result with HRESULT S_OK — there's no native error path, unlike
// WKWebView's NSError-on-throw. The user script is wrapped in a try/catch
// IIFE that prefixes its return value with "OK"/"ER" so failures route into
// promise.reject() the way macOS callers already expect.
Threads::Async<std::string> WebView::callJS(const std::string& script)
{
    auto promise = Threads::AsyncPromise<std::string>();

    if constexpr (Platform::isWindows())
    {
        auto wrapped = std::string {"(function() { try { var __r = eval("}
                       + jsStringLiteral(script)
                       + "); return 'OK' + (typeof __r === 'string' ? __r :"
                         " __r === undefined ? '' : JSON.stringify(__r)); }"
                         " catch (e) { return 'ER' + String("
                         "e && e.message ? e.message : e); } })()";

        evaluateJavaScript(
            wrapped,
            [promise](const std::string& result, const std::string& error)
            {
                if (!error.empty())
                {
                    promise.reject(error);
                    return;
                }
                if (result.size() >= 2 && result.substr(0, 2) == "OK")
                    promise.resolve(result.substr(2));
                else if (result.size() >= 2 && result.substr(0, 2) == "ER")
                    promise.reject(result.substr(2));
                else
                    promise.resolve(result);
            });
    }
    else
    {
        evaluateJavaScript(
            script,
            [promise](const std::string& result, const std::string& error)
            {
                if (error.empty())
                    promise.resolve(result);
                else
                    promise.reject(error);
            });
    }

    return promise.get();
}
} // namespace eacp::Graphics
