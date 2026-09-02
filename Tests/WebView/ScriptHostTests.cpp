#include <NanoTest/NanoTest.h>

#include <eacp/WebView/WebView/Bridge.h>
#include <eacp/WebView/WebView/ScriptHost.h>
#include <eacp/WebView/WebView/StateBridge.h>

#include <thread>

// The bridge with no web view under it. This executable links
// eacp-webview-bridge and nothing else, so it is also the proof that the
// target stands on its own: no WKWebView, no WebView2, no window, no page —
// just a ScriptHost that records what the bridge asks of it, and the exact
// strings that come back out.
//
// Every expectation here is a literal wire string rather than a re-derivation,
// because a host that renders and scripts HTML itself has to produce these
// same strings for a page to behave the same over both.

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

struct Label
{
    std::string text;

    MIRO_REFLECT(text)
};

class LabelStore : public EA::Broadcaster
{
public:
    const Label& get() const { return state; }

    void set(std::string text)
    {
        state.text = std::move(text);
        trigger();
    }

private:
    Label state;
};

// A ScriptHost that runs nothing: it keeps the user scripts it was given,
// the message handlers by name, and every script the bridge evaluated.
// armFileDrag is deliberately not overridden — the built-in command has to
// resolve against ScriptHost's default no-op.
class FakeScriptHost : public ScriptHost
{
public:
    struct UserScript
    {
        std::string source;
        bool atDocumentStart = false;
    };

    void addUserScript(const std::string& source, bool atDocumentStart) override
    {
        userScripts.add(UserScript {source, atDocumentStart});
    }

    void addScriptMessageHandler(const std::string& name,
                                 const MessageFunc& handler) override
    {
        handlers[name] = handler;
    }

    void removeScriptMessageHandler(const std::string& name) override
    {
        handlers.erase(name);
    }

    void evaluateJavaScript(const std::string& script) override
    {
        scripts.add(script);
    }

    bool routes(const std::string& name) const { return handlers.contains(name); }

    // What the shim's post() does: hand the channel a raw JSON string.
    void post(const std::string& name, const std::string& body)
    {
        if (auto it = handlers.find(name); it != handlers.end())
            it->second(body);
    }

    std::string lastScript() const
    {
        return scripts.empty() ? std::string {} : scripts.back();
    }

    Vector<UserScript> userScripts;
    Vector<std::string> scripts;

private:
    std::unordered_map<std::string, std::function<void(const std::string&)>>
        handlers;
};

constexpr auto bridgeChannel = "__eacpBridge";
constexpr auto replyTimeout = Time::MS {5000};

bool pumpUntilScripts(const FakeScriptHost& host, int count)
{
    return Threads::runEventLoopUntil([&] { return host.scripts.size() >= count; },
                                      replyTimeout);
}

void addEcho(WebViewBridge& transport)
{
    transport.getBridge().on<Message, Message>(
        "echo",
        std::function {[](const Message& m) { return Message {m.text + "!"}; }});
}

LabelStore& labelStore()
{
    static auto store = LabelStore {};
    return store;
}
} // namespace

EACP_STATE(Label, labelStore, label)

auto tShimIsTheUserScript = test("ScriptHost/shim/injectedAtDocumentStart") = []
{
    auto host = FakeScriptHost {};
    auto transport = WebViewBridge {host};

    check(host.userScripts.size() == 1);
    check(host.userScripts[0].atDocumentStart);
    check(host.userScripts[0].source
          == ResEmbed::get("bridge-shim.js", "EacpWebView").toString());
    check(host.routes(bridgeChannel));
    check(host.scripts.empty());
};

auto tSyncCommandDelivers = test("ScriptHost/command/syncResultReachesThePage") = []
{
    auto host = FakeScriptHost {};
    auto transport = WebViewBridge {host};
    addEcho(transport);

    host.post(bridgeChannel, R"({"id":1,"command":"echo","payload":{"text":"hi"}})");

    check(pumpUntilScripts(host, 1));
    check(host.lastScript()
          == R"(window.__eacp&&window.__eacp.deliver(1,{"text":"hi!"},null);)");
};

auto tAsyncCommandDelivers =
    test("ScriptHost/command/asyncResultReachesThePage") = []
{
    auto host = FakeScriptHost {};
    auto transport = WebViewBridge {host};

    transport.getBridge().onAsync<Message, Message>(
        "slow",
        std::function<void(const Message&, Miro::Completer<Message>)> {
            [](const Message& m, Miro::Completer<Message> complete)
            {
                std::thread([m, complete]
                            { complete.resolve(Message {m.text + "-done"}); })
                    .detach();
            }});

    host.post(bridgeChannel, R"({"id":2,"command":"slow","payload":{"text":"go"}})");

    check(pumpUntilScripts(host, 1));
    check(host.lastScript()
          == R"(window.__eacp&&window.__eacp.deliver(2,{"text":"go-done"},null);)");
};

auto tUnknownCommandRejects = test("ScriptHost/command/unknownNameRejects") = []
{
    auto host = FakeScriptHost {};
    auto transport = WebViewBridge {host};

    host.post(bridgeChannel, R"({"id":3,"command":"nope","payload":null})");

    check(pumpUntilScripts(host, 1));
    check(
        host.lastScript()
        == R"(window.__eacp&&window.__eacp.deliver(3,null,"unknown command: nope");)");
};

auto tArmFileDragResolves =
    test("ScriptHost/command/armFileDragResolvesWithNoDrag") = []
{
    auto host = FakeScriptHost {};
    auto transport = WebViewBridge {host};

    host.post(
        bridgeChannel,
        R"({"id":4,"command":"armFileDrag","payload":{"files":[{"path":"/a","name":"a"}]}})");

    check(pumpUntilScripts(host, 1));
    check(host.lastScript()
          == R"(window.__eacp&&window.__eacp.deliver(4,null,null);)");
};

auto tStateBroadcast = test("ScriptHost/state/broadcastReachesThePage") = []
{
    auto host = FakeScriptHost {};
    auto transport = WebViewBridge {host};

    labelStore().set("ready");

    check(host.lastScript()
          == R"(window.__eacp&&window.__eacp.dispatch("label",{"text":"ready"});)");
};

auto tCallExposedFunction = test("ScriptHost/call/replyResolvesTheAsync") = []
{
    auto host = FakeScriptHost {};
    auto transport = WebViewBridge {host};

    auto pending = transport.call("summarize", Miro::toJSON(Message {"x"}));

    check(
        host.lastScript()
        == R"(window.__eacp&&window.__eacp.callFunction(1,"summarize",{"text":"x"});)");

    host.post(bridgeChannel, R"({"reply":1,"result":{"text":"ok"}})");

    auto result = pending.waitFor(replyTimeout);
    check(result["text"].asString() == "ok");
};

auto tDestroyedBridgeSettlesNothing =
    test("ScriptHost/lifetime/commandInFlightIsDropped") = []
{
    auto host = FakeScriptHost {};

    {
        auto transport = WebViewBridge {host};
        addEcho(transport);
        host.post(bridgeChannel,
                  R"({"id":1,"command":"echo","payload":{"text":"hi"}})");
    }

    check(host.scripts.empty());
    check(!host.routes(bridgeChannel));

    Threads::runEventLoopFor(Time::MS {300});
    check(host.scripts.empty());
};
