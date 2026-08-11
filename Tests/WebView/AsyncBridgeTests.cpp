#include "Common.h"

#include <thread>
using namespace nano;

using eacp::Graphics::CommandExecution;
using eacp::Graphics::resolveWith;
using eacp::Graphics::runCommand;

namespace
{
struct EchoRequest
{
    std::string text;

    MIRO_REFLECT(text)
};

struct EchoResponse
{
    std::string echoed;

    MIRO_REFLECT(echoed)
};

// An ordinary synchronous API; the bridge is what makes calls to it async.
class EchoApi
{
public:
    void reflect(Miro::ApiReflector& r) { r.command(&EchoApi::echo, "echo"); }

    EchoResponse echo(const EchoRequest& req) const { return {req.text + "!"}; }
};

// The handler returns immediately and settles the Completer later, from a
// worker thread it owns.
class AsyncEchoApi
{
public:
    void reflect(Miro::ApiReflector& r)
    {
        r.command(&AsyncEchoApi::echoAsync, "echoAsync");
        r.command(&AsyncEchoApi::boomAsync, "boomAsync");
    }

    void echoAsync(const EchoRequest& req, Miro::Completer<EchoResponse> done)
    {
        std::thread([req, done] { done.resolve({req.text + "!"}); }).detach();
    }

    void boomAsync(const EchoRequest&, Miro::Completer<EchoResponse> done)
    {
        std::thread([done] { done.reject("kaboom"); }).detach();
    }
};

auto dispatchInvoke(Miro::Bridge& bridge, std::string command, std::string text)
{
    return [&bridge, command = std::move(command), text = std::move(text)](
               Miro::Resolve resolve)
    {
        auto payload = Miro::toJSON(EchoRequest {text});
        bridge.dispatchAsync(command, payload, resolve);
    };
}

auto echoInvoke(Miro::Bridge& bridge, std::string text)
{
    return dispatchInvoke(bridge, "echo", std::move(text));
}
} // namespace

auto tDeferredResolvesWithHandlerResult =
    test("AsyncBridge/mainThreadDeferred/resolvesWithSyncHandlerResult") = []
{
    auto api = EchoApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    auto work =
        runCommand(CommandExecution::MainThreadDeferred, echoInvoke(bridge, "hi"));

    auto result = work.waitFor(eacp::Time::MS {1000});

    check(result.isObject());
    check(result["echoed"].asString() == "hi!");
};

auto tWorkerThreadResolvesWithHandlerResult =
    test("AsyncBridge/workerThread/resolvesWithSyncHandlerResult") = []
{
    auto api = EchoApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    auto work =
        runCommand(CommandExecution::WorkerThread, echoInvoke(bridge, "worker"));

    auto result = work.waitFor(eacp::Time::MS {1000});

    check(result.isObject());
    check(result["echoed"].asString() == "worker!");
};

auto tDeferredDoesNotRunInline =
    test("AsyncBridge/mainThreadDeferred/doesNotRunBeforeLoopPumps") = []
{
    auto api = EchoApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    auto delivered = std::optional<std::string> {};
    auto failed = std::optional<std::string> {};

    auto work = runCommand(CommandExecution::MainThreadDeferred,
                           echoInvoke(bridge, "later"));

    resolveWith(std::move(work),
                [&](const Miro::Json::Value& result, const std::string* error)
                {
                    if (error != nullptr)
                        failed = *error;
                    else
                        delivered = result["echoed"].asString();
                });

    // onMessage's caller has returned to the event loop before the handler runs.
    check(!delivered.has_value());
    check(!failed.has_value());

    eacp::Threads::runEventLoopUntil([&] { return delivered || failed; },
                                     eacp::Time::MS {1000});

    check(delivered.has_value());
    check(*delivered == "later!");
    check(!failed.has_value());
};

auto tAsyncHandlerResolvesLater =
    test("AsyncBridge/asyncHandler/resolvesFromWorkerThread") = []
{
    auto api = AsyncEchoApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    // Invoked on the main loop, settled later from its own worker thread.
    auto work = runCommand(CommandExecution::MainThreadDeferred,
                           dispatchInvoke(bridge, "echoAsync", "later"));

    auto result = work.waitFor(eacp::Time::MS {1000});

    check(result.isObject());
    check(result["echoed"].asString() == "later!");
};

auto tAsyncHandlerRejects =
    test("AsyncBridge/asyncHandler/rejectSurfacesAsError") = []
{
    auto api = AsyncEchoApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    auto work = runCommand(CommandExecution::MainThreadDeferred,
                           dispatchInvoke(bridge, "boomAsync", "x"));

    auto threw = false;
    try
    {
        work.waitFor(eacp::Time::MS {1000});
    }
    catch (const eacp::Threads::AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()} == "kaboom");
    }

    check(threw);
};

auto tUnknownCommandRejects = test("AsyncBridge/unknownCommand/surfacesAsError") = []
{
    auto api = EchoApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    auto work = runCommand(CommandExecution::MainThreadDeferred,
                           [&bridge](Miro::Resolve resolve)
                           { bridge.dispatchAsync("missing", {}, resolve); });

    auto threw = false;
    try
    {
        work.waitFor(eacp::Time::MS {1000});
    }
    catch (const eacp::Threads::AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()}.find("unknown command") != std::string::npos);
    }

    check(threw);
};
