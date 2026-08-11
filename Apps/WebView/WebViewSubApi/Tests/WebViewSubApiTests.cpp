#include "App.h"

#include <eacp/WebView/Test/TestApp.h>

// Covers configureBridge({prefix}) in WebView/Resources/EacpBackend.ts.template:
// RootApi mounts GreeterApi as `nested`, so commands arrive as "nested.greet",
// while web/src/main.ts calls a plain backend.greet().

using namespace eacp::WebView::Test;

using nano::check;

namespace
{
// Asks the page to invoke one raw command name and report what happened
// (probeCommand in web/src/main.ts).
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
constexpr auto earlyTickSelector = R"([data-testid="early-tick-count"])";

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

auto tPrefixedInvokeReachesNestedCommand =
    test("SubApi/prefixedInvokeReachesNestedCommand") = []
{
    check(driver().waitFor(greetingSelector));
    check(driver().text(greetingSelector) == "hello world");
    check(app().root.nested.greetedName() == "world");
};

// The page subscribed to 'ticks', which must have registered as "nested.ticks"
// or this event would never reach it.
auto tPrefixedSubscriptionReceivesNestedEvent =
    test("SubApi/prefixedSubscriptionReceivesNestedEvent") = []
{
    app().root.nested.publishTick(7);

    check(driver().waitFor(tickSelector));
    check(driver().text(tickSelector) == "7");
};

// This handler bound to "ticks" before configureBridge ran (see
// web/src/main.ts); configureBridge re-points live subscriptions at the new
// prefix, which is what lets the event still arrive.
auto tSubscriptionMadeBeforeConfigureIsRebound =
    test("SubApi/subscriptionMadeBeforeConfigureIsRebound") = []
{
    app().root.nested.publishTick(9);

    check(driver().waitFor(earlyTickSelector));
    check(driver().text(earlyTickSelector) == "9");
};

// Both pin the wire shape by exact name, which works because `expose` is not
// prefixed: the page registers probeCommand through the generated expose().
auto tRootNameIsNotServed =
    test("SubApi/unprefixedNameIsNotServed") = [] { check(!probe("greet").served); };

auto tPrefixedNameIsServed = test("SubApi/prefixedNameIsServedOverTheWire") = []
{
    auto result = probe("nested.greet");

    check(result.served);
    check(result.text == "hello wire");
};
