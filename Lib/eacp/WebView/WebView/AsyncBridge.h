#pragma once

#include "../Common.h"

namespace eacp::Graphics
{

// Runs `work` on a detached worker thread. The seam runCommand uses for
// CommandExecution::WorkerThread, defined out of line so this header
// doesn't need <thread>.
void runOnWorkerThread(Callback work);

// Selects how the bridge executes a command when turning a TypeScript
// call into an async one. The C++ handler is always an ordinary
// synchronous function — this only chooses where/when the bridge runs
// it. TypeScript sees a Promise either way (it always has).
enum class CommandExecution
{
    // Default. Run the handler on a later main-thread tick, then deliver
    // the result. Safe for handlers that assume the main thread (the
    // common case for UI/state handlers); "async" only in the JS sense —
    // it doesn't block the current native callback, but it isn't parallel.
    MainThreadDeferred,

    // Run the handler on a worker thread, then marshal the result back to
    // the main thread to deliver. True off-main execution for slow work —
    // but the handler now runs concurrently with the main thread, so it
    // must be thread-safe with respect to anything the main thread touches
    // (e.g. shared state it reads/writes).
    WorkerThread
};

// Runs `invoke` under the chosen execution mode and exposes its outcome
// as an eacp Async. `invoke` is handed a Miro::Resolve and must call it
// once with the result (or an error) — in practice it just forwards to
// Miro::Bridge::dispatchAsync. This is the seam the framework is built
// around: Miro stays event-loop-agnostic (it only calls a std::function
// on completion); eacp owns the threading and composes its Async on top.
// The returned Async always settles on the main thread, so continuations
// (delivery to the WebView) run where the transport lives.
template <typename Invoke>
Threads::Async<Miro::Json::Value> runCommand(CommandExecution mode, Invoke invoke)
{
    auto promise = Threads::AsyncPromise<Miro::Json::Value> {};

    // Whatever thread `invoke` settles on, hop back to the main thread to
    // resolve — AsyncPromise (and the delivery that follows) is
    // main-thread only.
    auto settle =
        [promise](const Miro::Json::Value& result, const std::string* error)
    {
        if (error != nullptr)
        {
            auto message = *error;
            Threads::callAsync([promise, message] { promise.reject(message); });
        }
        else
        {
            auto value = result;
            Threads::callAsync([promise, value] { promise.resolve(value); });
        }
    };

    if (mode == CommandExecution::WorkerThread)
        runOnWorkerThread([invoke = std::move(invoke), settle]() mutable
                          { invoke(settle); });
    else
        Threads::callAsync([invoke = std::move(invoke), settle]() mutable
                           { invoke(settle); });

    return promise.get();
}

// Composes an Async<Json> onto a Miro::Resolve completion: when the work
// settles (on the main thread) the completion fires with the JSON result
// or the error message. The bridge wires this to deliver(id, ...).
inline void resolveWith(Threads::Async<Miro::Json::Value> work,
                        Miro::Resolve completion)
{
    work.then([completion](Miro::Json::Value value) { completion(value, nullptr); },
              [completion](const std::string& error)
              { completion(Miro::Json::Value {}, &error); });
}

// Maps an Async<Json> to a typed Async<Res> by deserializing the JSON
// through Miro when it resolves. A deserialization failure becomes a
// rejection rather than a thrown exception escaping the continuation.
// Used by WebViewBridge::call's typed overloads.
template <typename Res>
Threads::Async<Res> mapJson(Threads::Async<Miro::Json::Value> work)
{
    auto promise = Threads::AsyncPromise<Res> {};

    work.then(
        [promise](const Miro::Json::Value& value)
        {
            try
            {
                promise.resolve(Miro::createFromJSON<Res>(value));
            }
            catch (const std::exception& e)
            {
                promise.reject(e.what());
            }
        },
        [promise](const std::string& error) { promise.reject(error); });

    return promise.get();
}

} // namespace eacp::Graphics
