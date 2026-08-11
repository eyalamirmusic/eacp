#pragma once

#include "AppDriver.h"

#include <NanoTest/NanoTest.h>

#include <type_traits>

namespace eacp::WebView::Test
{
using Threads::Async;
using Threads::AsyncError;

// Test fixture holding a live T plus an AppDriver wired to its WebView and
// bridge, both rebuilt on restart(). Build one via createTestApp<T>(). T must
// expose a `Graphics::WebView webView` and `Graphics::WebViewBridge transport`.
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

    // Runs after every construction, including the already-built fixture.
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
// One type-erased restart callback per fixture type, fired before each test.
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

// Returns the process-singleton TestApp<T>; arguments past the first call
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

// Drop-in nano::test replacement: restarts every registered fixture before
// the body runs, and waits on Async<>-returning coroutine bodies.
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
