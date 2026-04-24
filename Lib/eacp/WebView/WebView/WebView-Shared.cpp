#include "WebView.h"

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
    return [category = std::move(category)](std::string_view path)
        -> std::optional<std::span<const std::uint8_t>>
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
    return [provider = std::move(provider),
            indexFile = std::move(indexFile)](std::string_view url)
        -> std::optional<ResourceResponse>
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

WebView::Options makeOptionsFromEmbedded(const WebView::EmbeddedOptions& embedded)
{
    WebView::Options options;
    options.schemes[embedded.scheme] =
        makeResourceProviderFromFiles(embedded.provider, embedded.indexFile);
    return options;
}
} // namespace

WebView::WebView()
    : WebView(Options {})
{
}

WebView::WebView(EmbeddedOptions options)
    : WebView(options.devServerURL.empty() ? makeOptionsFromEmbedded(options)
                                           : Options {})
{
    if (! options.autoLoad)
        return;

    if (! options.devServerURL.empty())
        loadURL(options.devServerURL);
    else
        loadURL(options.scheme + "://" + options.host + "/" + options.indexFile);
}
} // namespace eacp::Graphics
