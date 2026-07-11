#include "Common.h"
// Exercises the bridge-side async model: C++ command handlers stay
// completely synchronous, and the bridge turns each call into an async
// one. runCommand runs a Miro dispatch under a chosen execution mode and
// exposes the outcome as an eacp Async; resolveWith composes that Async
// onto the Miro::Resolve completion the wire delivers through. Miro itself
// is event-loop-agnostic — it only invokes the Resolve std::function.
//
// These drive the real event loop via Async::waitFor / runEventLoopUntil,
// the same way Tests/Core/AsyncTests.cpp does in a bare NanoTest main.

using namespace nano;
using namespace std::chrono_literals;

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

// A perfectly ordinary synchronous API — no Promise, no threading. The
// bridge is what makes calls to it async.
class EchoApi
{
public:
    void reflect(Miro::ApiReflector& r) { r.command(&EchoApi::echo, "echo"); }

    EchoResponse echo(const EchoRequest& req) const { return {req.text + "!"}; }
};

// A genuinely async API: the handler returns immediately and settles the
// Completer later, from a worker thread it owns. The bridge composes its
// Async on top of that deferred completion without any special casing —
// dispatchAsync simply hands the handler the Resolve.
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

// Invokes Miro's completion-based dispatch for `command` with the given
// text. This is exactly what WebViewBridge::onMessage hands to runCommand.
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

    auto result = work.waitFor(1s);

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

    auto result = work.waitFor(1s);

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

    // Deferred: nothing has run yet — onMessage's caller has returned to
    // the event loop before the handler executes.
    check(!delivered.has_value());
    check(!failed.has_value());

    eacp::Threads::runEventLoopUntil([&] { return delivered || failed; }, 1s);

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

    // MainThreadDeferred: the handler is *invoked* on the main loop, then
    // settles later from its own worker thread — the bridge's Async still
    // resolves on the main thread.
    auto work = runCommand(CommandExecution::MainThreadDeferred,
                           dispatchInvoke(bridge, "echoAsync", "later"));

    auto result = work.waitFor(1s);

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
        work.waitFor(1s);
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
        work.waitFor(1s);
    }
    catch (const eacp::Threads::AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()}.find("unknown command") != std::string::npos);
    }

    check(threw);
};
