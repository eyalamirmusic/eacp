#include "DebugControl.h"

#include "EacpQAScript.generated.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <sstream>
#include <string>

namespace eacp::Graphics
{
namespace
{
constexpr auto REQUEST_TIMEOUT = std::chrono::seconds(15);

std::string jsonEscape(const std::string& value)
{
    auto out = std::string {};
    out.reserve(value.size() + 2);
    out += '"';
    for (auto c: value)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    auto buf = std::ostringstream {};
                    buf << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                        << static_cast<int>(c);
                    out += buf.str();
                }
                else
                {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

eacp::HTTP::Response jsonResponse(int statusCode, const std::string& body)
{
    auto response = eacp::HTTP::Response {};
    response.statusCode = statusCode;
    response.setContent(body, "application/json; charset=utf-8");
    return response;
}

eacp::HTTP::Response jsonError(int statusCode, const std::string& message)
{
    return jsonResponse(statusCode, "{\"error\":" + jsonEscape(message) + "}");
}

struct JsResult
{
    std::string result;
    std::string error;
};

struct SnapResult
{
    EA::Vector<std::uint8_t> png;
    std::string error;
};
} // namespace

DebugControl::DebugControl(WebView& webViewToUse, int portToUse)
    : webView(webViewToUse)
    , port(portToUse)
    , server({eacp::HTTP::ServerThreadingMode::ThreadPool, 2})
{
}

bool DebugControl::start()
{
    // Covers future full document loads (document-start injection)...
    webView.addUserScript(kEacpQAScript, true);
    // ...the page that's already loaded right now...
    webView.evaluateJavaScript(kEacpQAScript);
    // ...and the initial load that may still be in flight (addUserScript can
    // register too late for it). The helper is idempotent, so chaining onto
    // any existing handler and re-injecting per load is harmless.
    auto previous = webView.onNavigationFinished;
    webView.onNavigationFinished = [this, previous](const std::string& url)
    {
        webView.evaluateJavaScript(kEacpQAScript);
        if (previous)
            previous(url);
    };

    server.post("/evaluate-javascript",
                [this](const eacp::HTTP::Request& request)
                { return handleEval(request); });
    server.get("/screenshot",
               [this](const eacp::HTTP::Request& request)
               { return handleScreenshot(request); });

    if (! server.listen(port))
    {
        eacp::LOG("[debug-control] failed to bind port " + std::to_string(port));
        return false;
    }

    eacp::LOG("[debug-control] listening on 127.0.0.1:"
              + std::to_string(server.boundPort()));
    return true;
}

eacp::HTTP::Response DebugControl::handleEval(const eacp::HTTP::Request& request)
{
    auto script = request.body;
    if (script.empty())
        return jsonError(400, "empty script");

    auto promise = std::make_shared<std::promise<JsResult>>();
    auto future = promise->get_future();

    // evaluateJavaScript must run on the main thread (WKWebView); this handler
    // is on a thread-pool worker, so marshal the call and block on the result.
    eacp::Threads::callAsync(
        [this, script, promise]
        {
            webView.evaluateJavaScript(
                script,
                [promise](const std::string& result, const std::string& error)
                { promise->set_value(JsResult {result, error}); });
        });

    if (future.wait_for(REQUEST_TIMEOUT) != std::future_status::ready)
        return jsonError(504, "evaluate-javascript timed out");

    auto outcome = future.get();
    if (! outcome.error.empty())
        return jsonResponse(200, "{\"error\":" + jsonEscape(outcome.error) + "}");

    return jsonResponse(200, "{\"result\":" + jsonEscape(outcome.result) + "}");
}

eacp::HTTP::Response DebugControl::handleScreenshot(const eacp::HTTP::Request&)
{
    auto promise = std::make_shared<std::promise<SnapResult>>();
    auto future = promise->get_future();

    eacp::Threads::callAsync(
        [this, promise]
        {
            webView.captureSnapshot(
                [promise](const EA::Vector<std::uint8_t>& png,
                          const std::string& error)
                { promise->set_value(SnapResult {png, error}); });
        });

    if (future.wait_for(REQUEST_TIMEOUT) != std::future_status::ready)
        return jsonError(504, "screenshot timed out");

    auto outcome = future.get();
    if (! outcome.error.empty())
        return jsonError(500, outcome.error);

    auto response = eacp::HTTP::Response {};
    response.statusCode = 200;
    response.setContent(std::string(reinterpret_cast<const char*>(outcome.png.data()),
                                    outcome.png.size()),
                        "image/png");
    return response;
}

EA::OwningPointer<DebugControl> DebugControl::startIfEnabled(WebView& webView)
{
    auto result = EA::OwningPointer<DebugControl> {};

    const char* flag = std::getenv("EACP_DEBUG_CONTROL");
    if (flag == nullptr || std::string(flag) == "0" || std::string(flag).empty())
        return result;

    result.create(webView, defaultPort);
    result->start();
    return result;
}
} // namespace eacp::Graphics
