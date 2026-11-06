#pragma once

/** @file
    Async<T>, a value that resolves later, with continuations run on the main thread.
*/

#include "EventLoop.h"
#include "ThreadUtils.h"

#include <coroutine>
#include <exception>
#include <stdexcept>
#include <type_traits>

namespace eacp::Threads
{

// Thrown by Async::waitFor and co_await when the Async was rejected or timed out.
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

    void settle()
    {
        auto fired = std::move(continuations);
        for (auto& c: fired)
            c();
    }

    Status status = Status::Pending;
    std::string error;
    Vector<Callback> continuations;
};
} // namespace detail

template <typename T>
class Async;

// The producer side of an Async. Every call is main thread only (asserted);
// only the first resolve or reject counts, later ones are ignored.
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

    // Leaves the Async pending forever and drops every continuation queued
    // on it, so nothing runs and nothing it captured is kept alive. For an
    // owner going away whose callers must not hear from it again.
    void abandon() const
    {
        assertMainThread();
        if (state->status != detail::AsyncState<T>::Status::Pending)
            return;
        state->continuations.clear();
    }

private:
    std::shared_ptr<detail::AsyncState<T>> state;
};

namespace detail
{
template <typename T>
struct PromiseReturn
{
    void return_value(T value) { promise.resolve(std::move(value)); }

    AsyncPromise<T> promise;
};

template <>
struct PromiseReturn<void>
{
    void return_void() { promise.resolve(); }

    AsyncPromise<void> promise;
};
} // namespace detail

// A value or error that arrives later, settled by an AsyncPromise on the main
// thread. Continuations run on the main thread, synchronously when it settles
// or at once if it already has. Also a coroutine return type: an exception
// escaping the coroutine rejects it with what().
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

    // Runs onValue or onError when it settles, or now if it already has.
    void then(OnValueCallback onValue, OnErrorCallback onError = {}) const
    {
        auto s = state;
        auto cont = [s,
                     valueCallback = std::move(onValue),
                     errorCallback = std::move(onError)]
        {
            if (s->status == detail::AsyncState<T>::Status::Resolved)
            {
                if (valueCallback)
                    invokeValue(*s, valueCallback);
            }
            else if (errorCallback)
            {
                errorCallback(s->error);
            }
        };

        if (s->status == detail::AsyncState<T>::Status::Pending)
            s->continuations.push_back(std::move(cont));
        else
            cont();
    }

    // Main thread only; pumps the event loop until the value settles. Off the
    // main thread debug builds assert and release builds are undefined; use
    // then(), or a wait of your own, there. Throws
    // AsyncError when rejected, on timeout or on an empty Async.
    T waitFor(Time::MS timeout)
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

    void pumpUntilSettled(Time::MS timeout)
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
        auto deadline = Time::Deadline {timeout};
        while (state->status == detail::AsyncState<T>::Status::Pending
               && !deadline.expired())
            runEventLoopFor(deadline.remaining());

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

// Resolves on the main thread after duration, through callAfter.
Async<void> delay(Time::MS duration);

} // namespace eacp::Threads
