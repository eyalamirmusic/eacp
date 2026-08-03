#include "App.h"

#include <eacp/WebView/Test/TestApp.h>

#include <string>

// Covers the intersection WebViewSubApi and WebViewTodo each cover one half
// of: a page that must call configureBridge({prefix}) because its api is
// mounted as a sub-API, AND whose subscriptions come from the generated React
// hooks module rather than from hand-written backend.on calls.
//
// The two store factories in EacpReact.ts.template (makeBridgeStore,
// makeKeyedStore) run their initial fetch and their backend.on() in the
// FACTORY BODY. The generated hooks module calls those factories at module
// scope, and ES imports are hoisted — so App.tsx's import of that module is
// fully evaluated before the first statement of main.tsx's body, which is
// where configureBridge is. The prefix is read at call time, and that call
// time is module evaluation: the store subscribes to 'counter' rather than
// 'nested.counter' and fetches 'getCounter' rather than 'nested.getCounter'.
//
// Neither miss reports anything. The fetch rejects into makeBridgeStore's own
// console.error, and the subscription simply never fires — there is no effect
// to re-run once the prefix arrives, so the value stays at the hook's
// generated initial.
//
// The third test is the control: `pulse` has no matching getter, so codegen
// picks makeNativeEvent, which subscribes inside a useEffect and therefore
// lands after render. Same page, same prefix, same generated client — it
// works. That is what pins the first two failures to WHEN the subscription is
// made rather than to prefixing being broken in general.
//
// A page can work around this by moving configureBridge into a module of its
// own and importing it ahead of anything that reaches the bridge, but that
// makes correctness depend on import order in every app that mounts a sub-API
// — which is the thing under test here.

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

// The fetch path. makeBridgeStore fetches once at module load, and the api
// seeds its counter away from Counter{} — so the seeded value rendering is
// proof the fetch resolved the prefix, and the hook's generated initial (0)
// rendering instead is proof it did not.
auto tStoreFetchUsesConfiguredPrefix =
    test("SubApiReact/storeInitialFetchUsesConfiguredPrefix") = []
{ check(driver().waitFor(counterShowing(Api::CounterApi::seededCounter))); };

// The subscribe path, driven from C++ so it does not depend on the fetch
// having worked: the store subscribed to 'counter', which must have
// registered as "nested.counter" or this event never reaches the page.
auto tStoreSubscriptionReceivesPrefixedEvent =
    test("SubApiReact/storeSubscriptionReceivesPrefixedEvent") = []
{
    app().root.nested.publishCounter(7);

    check(driver().waitFor(counterShowing(7)));
};

// The control. Identical wire arrangement, identical prefix, but this hook
// subscribes from a useEffect — so it is already correct today, and stays
// correct after the store factories are fixed.
auto tEffectSubscriptionReceivesPrefixedEvent =
    test("SubApiReact/effectSubscriptionReceivesPrefixedEvent") = []
{
    app().root.nested.publishPulse(3);

    check(driver().waitFor(pulseShowing(3)));
};
