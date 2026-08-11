#include "App.h"

#include <eacp/WebView/Test/TestApp.h>

#include <string>

// A sub-API page whose subscriptions come from the generated React hooks
// module: the store factories in EacpReact.ts.template fetch and subscribe at
// module scope, i.e. before main.tsx gets to call configureBridge({prefix}).

using namespace eacp::WebView::Test;

using nano::check;

namespace
{
constexpr auto readySelector = R"([data-testid="ready"])";

std::string showing(const std::string& testId, int value)
{
    return R"([data-testid=")" + testId + R"("][data-value=")"
           + std::to_string(value) + R"("])";
}

std::string counterShowing(int value)
{
    return showing("counter", value);
}

std::string pulseShowing(int beat)
{
    return showing("pulse", beat);
}

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
} // namespace

// The seeded value rendering proves the module-load fetch resolved the prefix;
// the hook's generated initial (0) would mean it did not.
auto tStoreFetchUsesConfiguredPrefix =
    test("SubApiReact/storeInitialFetchUsesConfiguredPrefix") = []
{ check(driver().waitFor(counterShowing(Api::CounterApi::seededCounter))); };

// The store subscribed to 'counter', which must have registered as
// "nested.counter" or this event never reaches the page.
auto tStoreSubscriptionReceivesPrefixedEvent =
    test("SubApiReact/storeSubscriptionReceivesPrefixedEvent") = []
{
    app().root.nested.publishCounter(7);

    check(driver().waitFor(counterShowing(7)));
};

// The control: same wire arrangement, but this hook subscribes from a
// useEffect, i.e. after configureBridge has run.
auto tEffectSubscriptionReceivesPrefixedEvent =
    test("SubApiReact/effectSubscriptionReceivesPrefixedEvent") = []
{
    app().root.nested.publishPulse(3);

    check(driver().waitFor(pulseShowing(3)));
};
