// Exercises the bridge-side async model: C++ command handlers stay
// completely synchronous, and the bridge turns each call into an async
// one. runCommand runs a Miro dispatch under a chosen execution mode and
// exposes the outcome as an eacp Async; resolveWith composes that Async
// onto the Miro::Resolve completion the wire delivers through. Miro itself
// is event-loop-agnostic — it only invokes the Resolve std::function.
//
// These drive the real event loop via Async::waitFor / runEventLoopUntil,
// the same way Tests/Core/AsyncTests.cpp does in a bare NanoTest main.

#include <eacp/WebView/WebView/AsyncBridge.h>

#include <Miro/Miro.h>
#include <NanoTest/NanoTest.h>

#include <chrono>
#include <optional>
#include <string>

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

// Invokes Miro's completion-based dispatch for "echo" with the given text.
// This is exactly what WebViewBridge::onMessage hands to runCommand.
auto echoInvoke(Miro::Bridge& bridge, std::string text)
{
    return [&bridge, text = std::move(text)](Miro::Resolve resolve)
    {
        auto payload = Miro::toJSON(EchoRequest {text});
        bridge.dispatchAsync("echo", payload, resolve);
    };
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
