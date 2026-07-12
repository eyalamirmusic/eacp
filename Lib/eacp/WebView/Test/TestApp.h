#pragma once

#include "AppDriver.h"

#include <NanoTest/NanoTest.h>

#include <type_traits>

namespace eacp::WebView::Test
{
using Threads::Async;
using Threads::AsyncError;

// Singleton-managed test fixture for a user app type T. Holds a
// live T plus the AppDriver wired to its WebView + bridge, and
// rebuilds both whenever restart() is called.
//
// Build one via createTestApp<T>(...) — that returns the process
// singleton and (optionally) installs a readiness check that runs
// after each rebuild. createTestApp also registers TestApp<T>'s
// restart() as a type-erased callback so the test() wrapper below
// can rebuild fixtures between tests without knowing about T.
//
// T must expose:
//   * `Graphics::WebView webView`            — the view tests drive
//   * `Graphics::WebViewBridge transport`    — to reach Miro::Bridge
template <typename T>
struct TestApp
{
    using ReadyCheck = std::function<void(AppDriver&)>;

    TestApp() { construct(); }

    void restart()
    {
        driverImpl.reset();
        instance.reset();
        construct();
        if (readyCheck)
            readyCheck(*driverImpl);
    }

    // Installs a check to run after each construction (initial and
    // subsequent restarts). Applied immediately to the current
    // fixture. Returns *this for chaining inside createTestApp.
    TestApp& onReady(ReadyCheck check)
    {
        readyCheck = std::move(check);

        if (readyCheck && driverImpl)
            readyCheck(*driverImpl);

        return *this;
    }

    T& app() { return *instance; }
    const T& app() const { return *instance; }

    AppDriver& driver() { return *driverImpl; }
    const AppDriver& driver() const { return *driverImpl; }

private:
    void construct()
    {
        instance.create();
        driverImpl.emplace(instance->webView, instance->transport.getBridge());
    }

    OwningPointer<T> instance;
    std::optional<AppDriver> driverImpl;
    ReadyCheck readyCheck;
};

namespace Detail
{
// Type-erased restart callbacks. createTestApp<T>() pushes one
// std::function<void()> per fixture type; the test() wrapper fires
// all of them before each body. Lives as a process singleton so the
// list survives static-init ordering quirks.
inline Vector<std::function<void()>>& restartRegistry()
{
    return Singleton::get<Vector<std::function<void()>>>();
}

inline void runAllRestarts()
{
    for (auto& cb: restartRegistry())
        cb();
}
} // namespace Detail

// Returns the process-singleton TestApp<T>. On first call, registers
// a type-erased restart hook so the test() wrapper rebuilds the
// fixture between tests, and (if a readySelector is given) installs
// a driver.waitFor(selector) check that runs after each construction.
// Subsequent calls return the same singleton — additional arguments
// are ignored.
template <typename T>
TestApp<T>& createTestApp(std::string_view readySelector = {})
{
    static auto& instance = [&]() -> TestApp<T>&
    {
        auto& self = Singleton::get<TestApp<T>>();

        if (!readySelector.empty())
        {
            self.onReady([sel = std::string {readySelector}](AppDriver& driver)
                         { driver.waitFor(sel); });
        }

        Detail::restartRegistry().add([]
                                      { Singleton::get<TestApp<T>>().restart(); });

        return self;
    }();
    return instance;
}

inline constexpr auto defaultTestTimeout = Time::MS {10000};

// Drop-in nano::test replacement that fires all registered fixture
// restarts before the body runs, and transparently handles both
// sync bodies and Async<>-returning coroutine bodies — the latter
// are waited on internally so the test author writes them as a
// single-scope coroutine.
//
// Use via `using namespace eacp::WebView::Test;` in test files —
// that brings test() into scope alongside TestApp / AppDriver /
// createTestApp.
struct TestProxy
{
    template <typename Fn>
    TestProxy& operator=(Fn body)
    {
        inner = [body = std::move(body)]
        {
            Detail::runAllRestarts();

            if constexpr (std::is_void_v<std::invoke_result_t<Fn>>)
                body();
            else
                body().waitFor(defaultTestTimeout);
        };
        return *this;
    }

    nano::TestProxy inner;
};

inline TestProxy test(std::string_view name)
{
    return {nano::test(name)};
}

} // namespace eacp::WebView::Test
