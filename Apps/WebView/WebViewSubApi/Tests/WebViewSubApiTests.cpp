#include "App.h"

#include <eacp/WebView/Test/TestApp.h>

// Covers the namespace-prefix escape hatch in the generated backend template
// (WebView/Resources/EacpBackend.ts.template).
//
// A generated TS client bakes in the command names of the api it was generated
// from. Mount that same api as a SUB-API of a larger one and every name gains a
// prefix on the wire, so the client can no longer reach it — which is why an
// embedded editor used to need its own WebView per api shape. configureBridge
// ({prefix}) lets one page host a client whose api hangs off a member of the
// root api instead.
//
// This app is that arrangement end to end: Api::RootApi exposes GreeterApi as
// `nested`, so its commands arrive as "nested.greet", while web/src/main.ts
// calls a plain backend.greet() after configureBridge({prefix: 'nested.'}).
//
// The DEFAULT (empty prefix) is not re-tested here — every other webview app
// and test in this repo generates the same template and would fail if the
// default ever stopped being a plain pass-through.

using namespace eacp::WebView::Test;

using nano::check;

namespace
{
// Asks the page to invoke one RAW command name and report what happened, so a
// test can assert the wire shape rather than infer it. See probeCommand in
// web/src/main.ts.
struct ProbeRequest
{
    std::string command;

    MIRO_REFLECT(command)
};

struct ProbeResult
{
    bool served = false;
    std::string text;

    MIRO_REFLECT(served, text)
};

constexpr auto readySelector = R"([data-testid="ready"])";
constexpr auto greetingSelector = R"([data-testid="greeting"])";
constexpr auto tickSelector = R"([data-testid="tick-count"])";

TestApp<MyApp>& testApp()
{
    static auto& instance = createTestApp<MyApp>(readySelector);
    return instance;
}

MyApp& app()
{
    return testApp().app();
}

AppDriver& driver()
{
    return testApp().driver();
}

eacp::Graphics::WebViewBridge& transport()
{
    return app().transport;
}

ProbeResult probe(const std::string& command)
{
    return transport()
        .call<ProbeResult>("probeCommand", ProbeRequest {command})
        .waitFor(eacp::Time::MS {5000});
}
} // namespace

// The invoke path: the page calls backend.greet(), which must arrive as
// "nested.greet". Asserted from both ends — the page gets its reply, and the
// C++ handler recorded the argument, so this cannot pass on a resolved-but-
// unhandled command.
auto tPrefixedInvokeReachesNestedCommand =
    test("SubApi/prefixedInvokeReachesNestedCommand") = []
{
    check(driver().waitFor(greetingSelector));
    check(driver().text(greetingSelector) == "hello world");
    check(app().root.nested.greetedName() == "world");
};

// The subscribe path, driven from C++ so it is independent of the invoke path:
// the page subscribed to 'ticks', which must have registered as "nested.ticks"
// or this event would never reach it.
auto tPrefixedSubscriptionReceivesNestedEvent =
    test("SubApi/prefixedSubscriptionReceivesNestedEvent") = []
{
    app().root.nested.publishTick(7);

    check(driver().waitFor(tickSelector));
    check(driver().text(tickSelector) == "7");
};

// Pins the wire shape the prefix exists to bridge. Without these, the tests
// above would still pass if Miro ever flattened sub-APIs onto the root — the
// prefix would then be doing nothing, and nothing would say so.
//
// Both also depend on `expose` NOT being prefixed: probeCommand is registered
// by the page through the generated expose() and called here by exact name.
auto tRootNameIsNotServed = test("SubApi/unprefixedNameIsNotServed") = []
{
    check(! probe("greet").served);
};

auto tPrefixedNameIsServed = test("SubApi/prefixedNameIsServedOverTheWire") = []
{
    auto result = probe("nested.greet");

    check(result.served);
    check(result.text == "hello wire");
};
