#include "WebView.h"

#include "DevServerProbe.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop.h>

#include <cstdio>
#include <string_view>

namespace eacp::Graphics
{
std::string mimeForPath(std::string_view path)
{
    if (path.ends_with(".html"))
        return "text/html; charset=utf-8";
    if (path.ends_with(".js"))
        return "application/javascript; charset=utf-8";
    if (path.ends_with(".css"))
        return "text/css; charset=utf-8";
    if (path.ends_with(".json"))
        return "application/json; charset=utf-8";
    if (path.ends_with(".svg"))
        return "image/svg+xml";
    if (path.ends_with(".png"))
        return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg"))
        return "image/jpeg";
    if (path.ends_with(".woff2"))
        return "font/woff2";
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
    auto query = path.find('?');

    if (query != std::string_view::npos)
        path = path.substr(0, query);

    return path.empty() ? std::string(indexFile) : std::string(path);
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

    auto url = useDevServer
                   ? embedded.devServerURL
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

namespace
{
#if defined(_WIN32)
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
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x",
                                  static_cast<unsigned char>(c));
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
#endif
} // namespace

Threads::Async<std::string> WebView::callJS(const std::string& script)
{
    auto promise = Threads::AsyncPromise<std::string>();

#if defined(_WIN32)
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
#else
    evaluateJavaScript(script,
                       [promise](const std::string& result, const std::string& error)
                       {
                           if (error.empty())
                               promise.resolve(result);
                           else
                               promise.reject(error);
                       });
#endif

    return promise.get();
}
} // namespace eacp::Graphics
