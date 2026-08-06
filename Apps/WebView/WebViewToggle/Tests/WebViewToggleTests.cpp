#include "App.h"

#include <eacp/WebView/Test/TestApp.h>

#include <string>

using namespace eacp::WebView::Test;

using nano::check;

namespace
{

constexpr auto ticksSelector = R"([data-testid="ticks"])";
constexpr auto pingSelector = R"([data-testid="ping"])";
constexpr auto pageClicksSelector = R"([data-testid="page-clicks"])";

// MyApp's WebView only exists while the panel is open, so the stock
// TestApp<T> fixture (which binds a driver to T::webView once, at
// construction) does not fit. This fixture owns the app and binds a fresh
// AppDriver around each open(); close() drops the driver first, because it
// holds references into the panel it is about to destroy.
struct ToggleFixture
{
    ToggleFixture() { instance.create(); }

    void restart()
    {
        driverImpl.reset();
        instance.reset();
        instance.create();
    }

    ToggleRoot& root() { return instance->root; }

    AppDriver& open()
    {
        root().open();
        driverImpl.emplace(root().panel->webView,
                           root().panel->transport.getBridge());
        return *driverImpl;
    }

    AppDriver& openAndWait()
    {
        auto& driver = open();
        driver.waitFor(ticksSelector);
        return driver;
    }

    void close()
    {
        driverImpl.reset();
        root().close();
    }

    OwningPointer<MyApp> instance;
    std::optional<AppDriver> driverImpl;
};

ToggleFixture& fixture()
{
    static auto& instance = []() -> ToggleFixture&
    {
        auto& self = Singleton::get<ToggleFixture>();
        Detail::restartRegistry().add(
            [] { Singleton::get<ToggleFixture>().restart(); });
        return self;
    }();
    return instance;
}

long long pageTicks(AppDriver& driver)
{
    return std::stoll(driver.text(ticksSelector));
}

// Each text() round-trip pumps the event loop, so polling doubles as
// letting the command -> state -> event -> render chain complete.
bool pageShowsClicks(AppDriver& driver, long long expected)
{
    for (auto attempt = 0; attempt < 100; ++attempt)
    {
        if (std::stoll(driver.text(pageClicksSelector)) == expected)
            return true;

        Threads::runEventLoopFor(Time::MS {10});
    }

    return false;
}

} // namespace

auto tOpenBridgesBothDirections =
    test("WebViewToggle/openBridgesBothDirections") = []
{
    auto& driver = fixture().openAndWait();

    auto first = pageTicks(driver);
    auto grew = false;

    for (auto attempt = 0; attempt < 100 && !grew; ++attempt)
    {
        Threads::runEventLoopFor(Time::MS {10});
        grew = pageTicks(driver) > first;
    }

    check(grew);

    driver.click(pingSelector);
    check(pageShowsClicks(driver, 1));
    check(fixture().root().panel->api.getState().pageClicks == 1);

    fixture().close();
    check(!fixture().root().isOpen());
};

auto tClosesWithTrafficInFlight =
    test("WebViewToggle/closesWithTrafficInFlight") = []
{
    auto& driver = fixture().openAndWait();

    // Fire a command and destroy the panel before its round trip lands; the
    // page's 50ms heartbeat is in flight too. The bridge teardown guards
    // must drop all of it without touching freed memory.
    driver.click(pingSelector);
    fixture().close();

    Threads::runEventLoopFor(Time::MS {200});
    check(!fixture().root().isOpen());
};

auto tReopensFresh = test("WebViewToggle/reopensFreshAfterClose") = []
{
    auto& first = fixture().openAndWait();
    first.click(pingSelector);
    check(pageShowsClicks(first, 1));
    fixture().close();

    // The reopened panel is a new WebView with a new api: zeroed state.
    auto& second = fixture().openAndWait();
    check(fixture().root().openCount == 2);
    check(pageShowsClicks(second, 0));

    second.click(pingSelector);
    check(pageShowsClicks(second, 1));

    fixture().close();
};

auto tSurvivesRapidCycles = test("WebViewToggle/survivesRapidOpenCloseCycles") = []
{
    // Destroy the WebView at varying points of its load, including before
    // the page exists at all — the dynamic-deletion path that regressed on
    // Mac before WebView.mm's deferred-hop hardening.
    for (auto cycle = 0; cycle < 5; ++cycle)
    {
        fixture().open();
        Threads::runEventLoopFor(Time::MS {cycle * 20});
        fixture().close();
    }

    Threads::runEventLoopFor(Time::MS {300});

    auto& driver = fixture().openAndWait();
    driver.click(pingSelector);
    check(pageShowsClicks(driver, 1));
    fixture().close();
};

auto tAppTeardownWhileOpen = test("WebViewToggle/appTeardownWhileOpen") = []
{
    fixture().openAndWait();

    // Left open on purpose: the fixture restart before the next test (and
    // process exit after the last) destroys the whole app with the panel
    // live — the everything-at-once RAII path.
};
