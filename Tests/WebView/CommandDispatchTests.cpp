#include "Common.h"

#include <thread>
// Drives the page -> C++ command path on a real WebView (window.eacp.invoke
// -> WebViewBridge::onMessage -> Miro dispatch). Covers the two features
// layered on top of the basic sync dispatch:
//
//   1. per-command execution mode — setCommandExecution(name, WorkerThread)
//      pushes a single command off the main thread while the rest stay on it
//   2. async Completer handlers — void(Req, Completer<Res>) that settle later
//      from a worker thread, resolving the page's invoke() Promise
//
// Both run end to end through the injected JS bridge shim, so they also
// exercise the wire round-trip, not just the C++ seam.

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
} // namespace

auto tPerCommandWorkerThread =
    test("CommandDispatch/perCommandMode/runsOnlyTaggedCommandOffMain") = []
{
    auto webView = WebView {};
    auto window = Window {};
    auto transport = WebViewBridge {webView};
    window.setContentView(webView);

    auto mainThread = std::this_thread::get_id();
    auto mainCmdThread = std::thread::id {};
    auto workerCmdThread = std::thread::id {};

    transport.getBridge().on<Message, Message>(
        "pingMain",
        std::function<Message(const Message&)> {[&](const Message& m)
                                                {
                                                    mainCmdThread =
                                                        std::this_thread::get_id();
                                                    return Message {m.text + "!"};
                                                }});

    transport.getBridge().on<Message, Message>(
        "pingWorker",
        std::function<Message(const Message&)> {[&](const Message& m)
                                                {
                                                    workerCmdThread =
                                                        std::this_thread::get_id();
                                                    return Message {m.text + "!"};
                                                }});

    transport.setCommandExecution("pingWorker", CommandExecution::WorkerThread);

    auto done = false;
    webView.addScriptMessageHandler("done",
                                    [&](const std::string&) { done = true; });

    webView.loadHTML(R"HTML(<!doctype html><html><body><script>
      Promise.all([
        window.eacp.invoke('pingMain', { text: 'm' }),
        window.eacp.invoke('pingWorker', { text: 'w' })
      ]).then(function () {
        window.webkit.messageHandlers.done.postMessage('done');
      });
    </script></body></html>)HTML");

    check(Threads::runEventLoopUntil([&] { return done; }, eacp::Time::MS {10000}));

    // The untagged command ran on the main loop; the tagged one ran on a
    // worker thread the bridge spawned just for it.
    check(mainCmdThread == mainThread);
    check(workerCmdThread != mainThread);
    check(workerCmdThread != std::thread::id {});
};

auto tAsyncCommandResolvesPageInvoke =
    test("CommandDispatch/asyncCommand/resolvesPageInvokeFromWorker") = []
{
    auto webView = WebView {};
    auto window = Window {};
    auto transport = WebViewBridge {webView};
    window.setContentView(webView);

    transport.getBridge().onAsync<Message, Message>(
        "slow",
        std::function<void(const Message&, Miro::Completer<Message>)> {
            [](const Message& m, Miro::Completer<Message> complete)
            {
                std::thread([m, complete]
                            { complete.resolve(Message {m.text + "-done"}); })
                    .detach();
            }});

    auto result = std::string {};
    auto done = false;
    webView.addScriptMessageHandler("result",
                                    [&](const std::string& body)
                                    {
                                        result = body;
                                        done = true;
                                    });

    webView.loadHTML(R"HTML(<!doctype html><html><body><script>
      window.eacp.invoke('slow', { text: 'go' }).then(function (r) {
        window.webkit.messageHandlers.result.postMessage(r.text);
      });
    </script></body></html>)HTML");

    check(Threads::runEventLoopUntil([&] { return done; }, eacp::Time::MS {10000}));
    check(result == "go-done");
};

auto tAsyncCommandRejectsPageInvoke =
    test("CommandDispatch/asyncCommand/rejectSurfacesAsPageRejection") = []
{
    auto webView = WebView {};
    auto window = Window {};
    auto transport = WebViewBridge {webView};
    window.setContentView(webView);

    transport.getBridge().onAsync<Message, Message>(
        "fail",
        std::function<void(const Message&, Miro::Completer<Message>)> {
            [](const Message&, Miro::Completer<Message> complete)
            { std::thread([complete] { complete.reject("nope"); }).detach(); }});

    auto error = std::string {};
    auto done = false;
    webView.addScriptMessageHandler("error",
                                    [&](const std::string& body)
                                    {
                                        error = body;
                                        done = true;
                                    });

    webView.loadHTML(R"HTML(<!doctype html><html><body><script>
      window.eacp.invoke('fail', { text: 'x' }).then(
        function () { window.webkit.messageHandlers.error.postMessage('resolved'); },
        function (e) { window.webkit.messageHandlers.error.postMessage(String(e.message)); }
      );
    </script></body></html>)HTML");

    check(Threads::runEventLoopUntil([&] { return done; }, eacp::Time::MS {10000}));
    check(error == "nope");
};
