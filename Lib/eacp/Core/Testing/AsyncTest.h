#pragma once

#include "../Threads/Async.h"

#include <NanoTest/NanoTest.h>

#include <chrono>
#include <functional>
#include <string_view>
#include <utility>

namespace eacp::Testing
{

inline constexpr auto defaultAsyncTestTimeout = std::chrono::seconds {10};

struct AsyncTestProxy
{
    template <typename Fn>
    AsyncTestProxy& operator=(Fn body)
    {
        inner = [body = std::move(body), timeout = timeout]
        {
            auto async = body();
            async.waitFor(timeout);
        };
        return *this;
    }

    nano::TestProxy inner;
    std::chrono::milliseconds timeout;
};

inline AsyncTestProxy asyncTest(
    std::string_view name,
    std::chrono::milliseconds timeout = defaultAsyncTestTimeout)
{
    return {nano::test(name), timeout};
}

} // namespace eacp::Testing
