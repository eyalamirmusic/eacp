#include "WebView.h"

#include "DevServerProbe.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>

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
    if (path.ends_with(".mp3"))
        return "audio/mpeg";
    if (path.ends_with(".wav"))
        return "audio/wav";
    if (path.ends_with(".aif") || path.ends_with(".aiff"))
        return "audio/aiff";
    if (path.ends_with(".flac"))
        return "audio/flac";
    if (path.ends_with(".m4a") || path.ends_with(".mp4"))
        return "audio/mp4";
    if (path.ends_with(".aac"))
        return "audio/aac";
    if (path.ends_with(".ogg") || path.ends_with(".opus"))
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

// `scheme:///abs/path?query#frag` -> `/abs/path`, percent-decoded. The host
// segment (between `://` and the next `/`) is ignored, so an empty host
// (`scheme:///`) yields a leading-slash absolute path as-is.
std::string pathFromFileURL(std::string_view url)
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

    return percentDecode(rest.substr(slash));
}

bool isUnderRoot(const std::filesystem::path& file,
                 const std::filesystem::path& root)
{
    auto ec = std::error_code {};
    auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    auto rel = std::filesystem::relative(file, canonicalRoot, ec);

    if (ec || rel.empty())
        return false;

    // A path that escapes the root resolves to a relative path starting
    // with "..". Anything else (including ".") is contained.
    return rel.native().rfind("..", 0) != 0;
}
} // namespace

ResourceProvider fromDisk(std::vector<std::string> allowedRoots)
{
    return [roots = std::move(allowedRoots)](
               std::string_view url) -> std::optional<ResourceResponse>
    {
        auto pathStr = pathFromFileURL(url);

        if (pathStr.empty())
            return std::nullopt;

        auto ec = std::error_code {};
        auto path = std::filesystem::weakly_canonical(pathStr, ec);

        if (ec)
            path = std::filesystem::path {pathStr};

        auto allowed = roots.empty()
                    || std::any_of(roots.begin(), roots.end(),
                                   [&](const auto& root)
                                   { return isUnderRoot(path, root); });

        if (!allowed || !std::filesystem::is_regular_file(path, ec))
            return std::nullopt;

        auto in = std::ifstream {path, std::ios::binary};

        if (!in)
            return std::nullopt;

        auto bytes = std::vector<std::uint8_t> {
            std::istreambuf_iterator<char> {in},
            std::istreambuf_iterator<char> {}};

        auto response = ResourceResponse {};
        response.mimeType = mimeForPath(path.string());
        response.data.assign(bytes.begin(), bytes.end());
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

Threads::Async<std::string> WebView::callJS(const std::string& script)
{
    auto promise = Threads::AsyncPromise<std::string>();
    evaluateJavaScript(script,
                       [promise](const std::string& result, const std::string& error)
                       {
                           if (error.empty())
                               promise.resolve(result);
                           else
                               promise.reject(error);
                       });
    return promise.get();
}
} // namespace eacp::Graphics
