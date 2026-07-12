#include "WebView.h"

#include "DevServerProbe.h"
#include "JsStringLiteral.h"
#include "StreamingRange.h"
#include "WebViewDetail.h"

namespace eacp::Graphics
{
std::string mimeForPath(std::string_view path)
{
    // Match on a lowercased copy so MyClip.WAV maps the same as .wav.
    auto lower = std::string {path};
    std::transform(lower.begin(),
                   lower.end(),
                   lower.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });

    auto endsWith = [&](std::string_view ext) { return lower.ends_with(ext); };

    // Text / web
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

    // Images
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

    // Fonts
    if (endsWith(".woff2"))
        return "font/woff2";

    // Audio
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

std::string pathFromURL(std::string_view url, std::string_view indexFile)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string_view::npos)
        return {};

    auto afterHost = url.find('/', schemeEnd + 3);

    if (afterHost == std::string_view::npos)
        return std::string(indexFile);

    auto path = url.substr(afterHost + 1);

    // WebKit hands custom-scheme handlers the full URL including any fragment
    // (it only strips fragments for standard schemes like http). A hash-routed
    // SPA loads e.g. app://local/index.html#/route — strip the fragment so the
    // resource key stays "index.html". Strip the query string the same way.
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

    for (auto i = std::size_t {0}; i < encoded.size(); ++i)
    {
        if (encoded[i] == '%' && i + 2 < encoded.size())
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

ResolvedRange resolveRangeHeader(std::string_view headerValue, RangeSize size)
{
    auto full = ResolvedRange {RangeRequest::Full, ByteRange {0, size}};
    auto unsatisfiable = ResolvedRange {RangeRequest::Unsatisfiable, ByteRange {}};

    constexpr auto prefix = std::string_view {"bytes="};

    if (!headerValue.starts_with(prefix))
        return full;

    auto spec = headerValue.substr(prefix.size());

    // We serve a single range only; a comma marks a multi-range request.
    auto dash = spec.find('-');

    if (spec.find(',') != std::string_view::npos || dash == std::string_view::npos)
        return full;

    // Any byte range over an empty resource is unsatisfiable.
    if (size == 0)
        return unsatisfiable;

    auto firstText = spec.substr(0, dash);
    auto lastText = spec.substr(dash + 1);

    // Suffix form `bytes=-N`: the last N bytes.
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

StreamingResponsePlan planStreamingResponse(std::string_view rangeHeader,
                                            const StreamingResource& resource)
{
    auto resolved = resolveRangeHeader(rangeHeader, resource.size);

    auto plan = StreamingResponsePlan {};
    plan.served = resolved.served;
    plan.statusCode = resource.statusCode;

    plan.headers.add({"Content-Type", resource.mimeType});
    // WebView2 enforces cross-origin rules on our own custom scheme, so fetch()
    // needs an explicit allow-origin header; kept here so both backends agree.
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

    // A Windows drive path arrives as "/C:/dir/file"; drop the leading slash
    // so it parses as the native "C:/dir/file". POSIX paths ("/var/...") have
    // no drive letter and are left untouched.
    if (decoded.size() >= 3 && decoded[0] == '/'
        && std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':')
        decoded.erase(0, 1);

    return decoded;
}

FileProvider fromResEmbed(std::string category)
{
    return [category = std::move(category)](
               std::string_view path) -> std::optional<std::span<const std::uint8_t>>
    {
        auto view = ResEmbed::get(std::string(path), category);

        if (!view)
            return std::nullopt;

        return std::span<const std::uint8_t> {view.data(), view.size()};
    };
}

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

        // Kept open behind a shared_ptr so the reader can pull chunks across
        // many scheme-task callbacks, then closes when the last reader drops.
        // File::isUnder canonicalises, so a raw path is fine here.
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
        { return file->read(offset, out); };
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

    // Headless test harnesses install user scripts and navigation
    // callbacks AFTER construction (TestApp wires the agent script
    // and AppDriver hook once the WebView already exists). Loading
    // inline would race those — the first navigation could fire
    // before the agent is registered. Defer to the next runloop
    // tick so the harness finishes wiring before the load starts.
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

namespace detail
{
Vector<WebView*>& registeredWebViews()
{
    static auto instances = Vector<WebView*>();
    return instances;
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
constexpr double minZoomLevel = 0.25;
constexpr double maxZoomLevel = 5.0;
constexpr double zoomStep = 1.1;
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

Threads::Async<std::string> WebView::callJS(const std::string& script)
{
    auto promise = Threads::AsyncPromise<std::string>();

    if (Platform::isWindows())
    {
        // WebView2's ExecuteScript reports JS exceptions as a "null" result
        // with HRESULT S_OK — there's no native error path, unlike WKWebView's
        // NSError-on-throw. Wrap the user script in a try/catch IIFE that
        // prefixes its return value with "OK"/"ER" so we can route failures
        // into promise.reject() the way macOS callers already expect.
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
