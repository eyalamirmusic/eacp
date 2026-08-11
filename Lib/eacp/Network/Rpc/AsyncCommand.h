#pragma once

#include "../Common.h"

#include <Miro/Bridge.h>
#include <Miro/Reflect.h>

// The execution glue every Miro transport shares: how a synchronous C++
// command handler becomes an async call on the wire, and how a JSON result
// becomes a typed one.
namespace eacp::Rpc
{

// Runs `work` on a detached worker thread. Out of line so this header doesn't
// need <thread>.
void runOnWorkerThread(Callback work);

// Where a bridge runs a command handler. The handler is an ordinary
// synchronous function either way; the caller always sees an async completion.
enum class CommandExecution
{
    // A later main-thread tick: not parallel, so handlers assuming the main
    // thread stay safe.
    MainThreadDeferred,

    // A worker thread, with the result marshalled back to the main thread.
    // The handler must be thread-safe against whatever the main thread
    // touches.
    WorkerThread
};

// `invoke` is handed a Miro::Resolve and must call it once with the result or
// an error. The returned Async always settles on the main thread, so
// continuations run where the transport lives.
template <typename Invoke>
Threads::Async<Miro::Json::Value> runCommand(CommandExecution mode, Invoke invoke)
{
    auto promise = Threads::AsyncPromise<Miro::Json::Value> {};

    // Whatever thread `invoke` settles on, hop back to the main thread:
    // AsyncPromise and the delivery that follows are main-thread only.
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

// Fires completion on the main thread with the JSON result or the error
// message. Transports wire this to their reply delivery.
inline void resolveWith(Threads::Async<Miro::Json::Value> work,
                        Miro::Resolve completion)
{
    work.then([completion](Miro::Json::Value value) { completion(value, nullptr); },
              [completion](const std::string& error)
              { completion(Miro::Json::Value {}, &error); });
}

// Deserializes through Miro on resolve; a failure becomes a rejection rather
// than an exception escaping the continuation.
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

} // namespace eacp::Rpc
