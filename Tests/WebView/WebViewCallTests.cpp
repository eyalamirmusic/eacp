#include "Common.h"
// Drives the C++ -> page call path on a real WKWebView: the page registers
// functions with window.eacp.expose(...), and the native side calls them via
// WebViewBridge::call(...), awaiting the resolved value. Covers a synchronous
// exposed function, an async (Promise-returning) one, the typed overload, and
// the error path — proving Miro/eacp can call async JavaScript from C++ and
// get the result back as an eacp Async.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;
using namespace std::chrono_literals;

namespace
{
struct Message
{
    std::string text;

    MIRO_REFLECT(text)
};

// Registers three exposed functions and signals readiness so the test
// only calls once window.eacp.expose has run. `echoAsync` returns a
// Promise that settles on a later tick — the case that plain
// evaluateJavaScript could never await.
const std::string pageHtml = R"HTML(<!doctype html><html><body><script>
  window.eacp.expose('echo', function (p) {
    return { text: p.text + '!' };
  });
  window.eacp.expose('echoAsync', function (p) {
    return new Promise(function (resolve) {
      setTimeout(function () { resolve({ text: p.text + '-async' }); }, 10);
    });
  });
  window.eacp.expose('boom', function () {
    throw new Error('kaboom');
  });
  window.webkit.messageHandlers.ready.postMessage('ready');
</script></body></html>)HTML";

// Builds a live WebView + bridge, loads the page, and pumps until the
// page has registered its exposed functions.
struct Fixture
{
    WebView webView {};
    Window window {};
    WebViewBridge transport {webView};
    bool ready = false;

    Fixture()
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.loadHTML(pageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; }, 10s));
    }
};
} // namespace

auto tCallSyncExposedFunction =
    test("WebViewCall/callsSynchronousExposedFunction") = []
{
    auto fix = Fixture {};

    auto result =
        fix.transport.call("echo", Miro::toJSON(Message {"hi"})).waitFor(10s);

    check(result.isObject());
    check(result["text"].asString() == "hi!");
};

auto tCallAsyncExposedFunction = test("WebViewCall/awaitsAsyncExposedFunction") = []
{
    auto fix = Fixture {};

    auto result =
        fix.transport.call("echoAsync", Miro::toJSON(Message {"hi"})).waitFor(10s);

    check(result.isObject());
    check(result["text"].asString() == "hi-async");
};

auto tCallTypedOverload = test("WebViewCall/typedOverloadRoundTrips") = []
{
    auto fix = Fixture {};

    auto reply = fix.transport.call<Message>("echo", Message {"yo"}).waitFor(10s);

    check(reply.text == "yo!");
};

auto tCallThrowingFunctionRejects =
    test("WebViewCall/exposedThrowSurfacesAsRejection") = []
{
    auto fix = Fixture {};

    auto threw = false;
    try
    {
        fix.transport.call("boom").waitFor(10s);
    }
    catch (const Threads::AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()}.find("kaboom") != std::string::npos);
    }

    check(threw);
};

auto tCallMissingFunctionRejects =
    test("WebViewCall/missingFunctionSurfacesAsRejection") = []
{
    auto fix = Fixture {};

    auto threw = false;
    try
    {
        fix.transport.call("nope").waitFor(10s);
    }
    catch (const Threads::AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()}.find("no exposed function")
              != std::string::npos);
    }

    check(threw);
};
