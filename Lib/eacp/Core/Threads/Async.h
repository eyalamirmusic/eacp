#pragma once

#include "EventLoop.h"
#include "ThreadUtils.h"

#include <eacp/Core/Utils/Containers.h>

#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace eacp::Threads
{

struct AsyncError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

namespace detail
{
template <typename T>
struct AsyncValue
{
    std::optional<T> value;
};

template <>
struct AsyncValue<void>
{
};

template <typename T>
struct OnValueCallbackType
{
    using type = std::function<void(T)>;
};

template <>
struct OnValueCallbackType<void>
{
    using type = Callback;
};

template <typename T>
struct AsyncState : AsyncValue<T>
{
    enum class Status
    {
        Pending,
        Resolved,
        Rejected
    };

    Status status = Status::Pending;
    std::string error;
    Vector<Callback> continuations;

    void settle()
    {
        auto fired = std::move(continuations);
        for (auto& c: fired)
            c();
    }
};
} // namespace detail

template <typename T>
class Async;

template <typename T = void>
class AsyncPromise
{
public:
    AsyncPromise()
        : state(std::make_shared<detail::AsyncState<T>>())
    {
    }

    Async<T> get() const { return Async<T> {state}; }

    template <typename Self = T>
        requires(!std::is_void_v<Self>)
    void resolve(Self value) const
    {
        assertMainThread();
        if (state->status != detail::AsyncState<T>::Status::Pending)
            return;
        state->value = std::move(value);
        state->status = detail::AsyncState<T>::Status::Resolved;
        state->settle();
    }

    template <typename Self = T>
        requires std::is_void_v<Self>
    void resolve() const
    {
        assertMainThread();
        if (state->status != detail::AsyncState<T>::Status::Pending)
            return;
        state->status = detail::AsyncState<T>::Status::Resolved;
        state->settle();
    }

    void reject(std::string message) const
    {
        assertMainThread();
        if (state->status != detail::AsyncState<T>::Status::Pending)
            return;
        state->error = std::move(message);
        state->status = detail::AsyncState<T>::Status::Rejected;
        state->settle();
    }

private:
    std::shared_ptr<detail::AsyncState<T>> state;
};

namespace detail
{
template <typename T>
struct PromiseReturn
{
    AsyncPromise<T> promise;

    void return_value(T value) { promise.resolve(std::move(value)); }
};

template <>
struct PromiseReturn<void>
{
    AsyncPromise<void> promise;

    void return_void() { promise.resolve(); }
};
} // namespace detail

template <typename T = void>
class Async
{
public:
    using ValueType = T;

    using OnValueCallback = typename detail::OnValueCallbackType<T>::type;
    using OnErrorCallback = std::function<void(const std::string&)>;

    Async() = default;

    bool isReady() const
    {
        return state && state->status != detail::AsyncState<T>::Status::Pending;
    }

    bool isResolved() const
    {
        return state && state->status == detail::AsyncState<T>::Status::Resolved;
    }

    bool isRejected() const
    {
        return state && state->status == detail::AsyncState<T>::Status::Rejected;
    }

    void then(OnValueCallback onValue, OnErrorCallback onError = {}) const
    {
        auto s = state;
        auto cont = [s, onValue = std::move(onValue), onError = std::move(onError)]
        {
            if (s->status == detail::AsyncState<T>::Status::Resolved)
            {
                if (onValue)
                    invokeValue(*s, onValue);
            }
            else if (onError)
            {
                onError(s->error);
            }
        };

        if (s->status == detail::AsyncState<T>::Status::Pending)
            s->continuations.push_back(std::move(cont));
        else
            cont();
    }

    T waitFor(std::chrono::milliseconds timeout)
    {
        pumpUntilSettled(timeout);
        throwIfRejected();

        if constexpr (!std::is_void_v<T>)
            return std::move(*state->value);
    }

    bool await_ready() const noexcept { return isReady(); }

    void await_suspend(std::coroutine_handle<> handle) const
    {
        state->continuations.push_back([handle] { handle.resume(); });
    }

    T await_resume()
    {
        throwIfRejected();
        if constexpr (!std::is_void_v<T>)
            return std::move(*state->value);
    }

    struct promise_type : detail::PromiseReturn<T>
    {
        Async<T> get_return_object() { return this->promise.get(); }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }

        void unhandled_exception()
        {
            try
            {
                std::rethrow_exception(std::current_exception());
            }
            catch (const std::exception& e)
            {
                this->promise.reject(e.what());
            }
            catch (...)
            {
                this->promise.reject("Unknown exception in Async coroutine");
            }
        }
    };

private:
    friend class AsyncPromise<T>;

    explicit Async(std::shared_ptr<detail::AsyncState<T>> s)
        : state(std::move(s))
    {
    }

    static void invokeValue(detail::AsyncState<T>& s, const OnValueCallback& cb)
    {
        if constexpr (std::is_void_v<T>)
            cb();
        else
            cb(*s.value);
    }

    void pumpUntilSettled(std::chrono::milliseconds timeout)
    {
        assertMainThread();
        if (!state)
            throw AsyncError {"Async::waitFor on empty Async"};

        if (state->status != detail::AsyncState<T>::Status::Pending)
            return;

        state->continuations.push_back([] { stopEventLoop(); });

        // Other code (e.g. a Window's onQuit-via-callAsync) may call
        // stopEventLoop from a queued callback that has nothing to do
        // with this Async. If that happens, runEventLoopFor returns
        // before our state has settled — re-enter and keep pumping
        // until either the state actually settles or the deadline
        // expires.
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (state->status == detail::AsyncState<T>::Status::Pending)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                break;

            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
            runEventLoopFor(remaining);
        }

        if (state->status == detail::AsyncState<T>::Status::Pending)
            throw AsyncError {"Async::waitFor timed out"};
    }

    void throwIfRejected()
    {
        if (state->status == detail::AsyncState<T>::Status::Rejected)
            throw AsyncError {state->error};
    }

    std::shared_ptr<detail::AsyncState<T>> state;
};

inline Async<void> delay(std::chrono::milliseconds duration)
{
    auto promise = AsyncPromise<void>();
    std::thread(
        [promise, duration]
        {
            std::this_thread::sleep_for(duration);
            callAsync([promise] { promise.resolve(); });
        })
        .detach();
    return promise.get();
}

} // namespace eacp::Threads
