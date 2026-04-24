#include <eacp/Network/HTTPServer/HttpServer.h>
#include <eacp/WebView/WebView.h>
#include <WebResources.h>

#include <chrono>
#include <iostream>
#include <string>

using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto apiPort = 8080;

HTTP::Response withCORS(HTTP::Response res)
{
    res.headers["Access-Control-Allow-Origin"] = "*";
    res.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
    res.headers["Access-Control-Allow-Headers"] = "Content-Type";
    return res;
}

long long currentEpochMillis()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

HTTP::Response handleApiRequest(const HTTP::Request& req)
{
    if (req.type == "OPTIONS")
    {
        auto res = HTTP::Response();
        res.statusCode = 204;
        return withCORS(std::move(res));
    }

    auto res = HTTP::Response();
    res.headers["Content-Type"] = "application/json";

    if (req.url == "/api/ping")
    {
        res.statusCode = 200;
        res.content = R"({"pong":true,"serverTimeMs":)"
                      + std::to_string(currentEpochMillis()) + "}";
    }
    else
    {
        res.statusCode = 404;
        res.content = R"({"error":"not found"})";
    }

    return withCORS(std::move(res));
}
} // namespace

struct MyApp
{
    MyApp()
    {
        setApplicationMenuBar(buildDefaultWebViewMenuBar());
        window.setContentView(webView);

        if (server.listen(apiPort, handleApiRequest))
            std::cout << "API server listening on http://localhost:" << apiPort
                      << "\n";
        else
            std::cerr << "Failed to start API server on port " << apiPort << "\n";
    }

    HTTP::Server server;
    WebView webView {embeddedOptions("WebApp")};
    Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();

    return 0;
}
