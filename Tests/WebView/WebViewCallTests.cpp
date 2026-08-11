#include "Common.h"
using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
struct Message
{
    std::string text;

    MIRO_REFLECT(text)
};

// `echoAsync` returns a Promise that settles on a later tick, the case plain
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
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }
};
} // namespace

auto tCallSyncExposedFunction =
    test("WebViewCall/callsSynchronousExposedFunction") = []
{
    auto fix = Fixture {};

    auto result = fix.transport.call("echo", Miro::toJSON(Message {"hi"}))
                      .waitFor(webViewResultTimeout);

    check(result.isObject());
    check(result["text"].asString() == "hi!");
};

auto tCallAsyncExposedFunction = test("WebViewCall/awaitsAsyncExposedFunction") = []
{
    auto fix = Fixture {};

    auto result = fix.transport.call("echoAsync", Miro::toJSON(Message {"hi"}))
                      .waitFor(webViewResultTimeout);

    check(result.isObject());
    check(result["text"].asString() == "hi-async");
};

auto tCallTypedOverload = test("WebViewCall/typedOverloadRoundTrips") = []
{
    auto fix = Fixture {};

    auto reply = fix.transport.call<Message>("echo", Message {"yo"})
                     .waitFor(webViewResultTimeout);

    check(reply.text == "yo!");
};

auto tCallThrowingFunctionRejects =
    test("WebViewCall/exposedThrowSurfacesAsRejection") = []
{
    auto fix = Fixture {};

    auto threw = false;
    try
    {
        fix.transport.call("boom").waitFor(webViewResultTimeout);
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
        fix.transport.call("nope").waitFor(webViewResultTimeout);
    }
    catch (const Threads::AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()}.find("no exposed function")
              != std::string::npos);
    }

    check(threw);
};
