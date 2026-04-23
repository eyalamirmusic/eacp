#include <eacp/WebView/WebView.h>

#include <ResEmbed/ResEmbed.h>
#include <WebResources.h>

using namespace eacp;
using namespace Graphics;

namespace
{
std::string mimeForPath(std::string_view path)
{
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js")) return "application/javascript; charset=utf-8";
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    if (path.ends_with(".json")) return "application/json; charset=utf-8";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
    if (path.ends_with(".woff2")) return "font/woff2";
    return "application/octet-stream";
}

std::string pathFromURL(std::string_view url)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string_view::npos)
        return {};

    auto afterHost = url.find('/', schemeEnd + 3);

    if (afterHost == std::string_view::npos)
        return "index.html";

    auto path = url.substr(afterHost + 1);
    auto query = path.find('?');

    if (query != std::string_view::npos)
        path = path.substr(0, query);

    return path.empty() ? "index.html" : std::string(path);
}

std::optional<ResourceResponse> serveEmbedded(std::string_view url)
{
    auto path = pathFromURL(url);
    auto view = ResEmbed::get(path, "WebApp");

    if (!view)
        return std::nullopt;

    ResourceResponse response;
    response.mimeType = mimeForPath(path);
    response.data.assign(view.data(), view.data() + view.size());
    return response;
}

WebView::Options makeOptions()
{
    WebView::Options options;
    options.schemes["app"] = &serveEmbedded;
    return options;
}
} // namespace

struct ParentView final : View
{
    ParentView()
    {
        addChildren({webView});
        webView.loadURL("app://local/index.html");
    }

    void resized() override { scaleToFit({webView}); }

    WebView webView {makeOptions()};
};

struct MyApp
{
    MyApp() { window.setContentView(parentView); }

    ParentView parentView;
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}
