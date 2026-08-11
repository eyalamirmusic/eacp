#include "Common.h"

#include <thread>
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

    check(Threads::runEventLoopUntil([&] { return done; }, firstNavigationTimeout));

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

    check(Threads::runEventLoopUntil([&] { return done; }, firstNavigationTimeout));
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

    check(Threads::runEventLoopUntil([&] { return done; }, firstNavigationTimeout));
    check(error == "nope");
};
