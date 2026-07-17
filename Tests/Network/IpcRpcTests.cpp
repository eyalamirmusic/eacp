#include "Common.h"

#include <eacp/Network/IPCRpc/RpcClient.h>
#include <eacp/Network/IPCRpc/RpcServer.h>

#include <optional>

using namespace nano;
using eacp::IPC::RpcClient;
using eacp::IPC::RpcServer;
using eacp::Threads::runEventLoopUntil;

namespace
{
constexpr auto pumpTimeout = eacp::Time::MS {10000};

struct AddRequest
{
    int a = 0;
    int b = 0;

    MIRO_REFLECT(a, b)
};

struct AddResult
{
    int sum = 0;

    MIRO_REFLECT(sum)
};

class MathApi
{
public:
    void reflect(Miro::ApiReflector& r) { r.command(&MathApi::add, "add"); }

    AddResult add(const AddRequest& request) const
    {
        return {request.a + request.b};
    }
};
} // namespace

// The call is issued while the dial is still in the air, so this also
// proves the outbox: nothing is lost to the connection race.
auto tTypedCall = test("Ipc/Rpc/typedCallRoundTrips") = []
{
    auto api = MathApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    auto server = RpcServer {"eacp.tests.rpc.add", bridge};
    auto client = RpcClient {"eacp.tests.rpc.add"};

    auto sum = 0;
    client.call<AddResult>("add", AddRequest {20, 22})
        .then([&](AddResult result) { sum = result.sum; },
              [](const std::string&) {});

    check(runEventLoopUntil([&] { return sum != 0; }, pumpTimeout));
    check(sum == 42);
};

auto tUnknownCommandRejects = test("Ipc/Rpc/unknownCommandRejects") = []
{
    auto bridge = Miro::Bridge {};
    auto server = RpcServer {"eacp.tests.rpc.unknown", bridge};
    auto client = RpcClient {"eacp.tests.rpc.unknown"};

    auto error = std::string {};
    client.call<AddResult>("nope", AddRequest {})
        .then([](AddResult) {}, [&](const std::string& reason) { error = reason; });

    check(runEventLoopUntil([&] { return !error.empty(); }, pumpTimeout));
};

auto tEventsReachEveryClient = test("Ipc/Rpc/eventsReachEveryClient") = []
{
    auto bridge = Miro::Bridge {};
    auto server = RpcServer {"eacp.tests.rpc.events", bridge};

    auto first = RpcClient {"eacp.tests.rpc.events"};
    auto second = RpcClient {"eacp.tests.rpc.events"};

    auto firstGot = 0;
    auto secondGot = 0;
    first.on<AddResult>("total",
                        [&](const AddResult& event) { firstGot = event.sum; });
    second.on<AddResult>("total",
                         [&](const AddResult& event) { secondGot = event.sum; });

    check(runEventLoopUntil([&] { return server.connectedClients() == 2; },
                            pumpTimeout));

    bridge.emit("total", AddResult {7});

    check(runEventLoopUntil([&] { return firstGot == 7 && secondGot == 7; },
                            pumpTimeout));
};

// A call must never hang forever: when the dial finds nobody, the pending
// promise is rejected rather than orphaned.
auto tCallsFailWhenNobodyServes = test("Ipc/Rpc/callsFailWhenNobodyServes") = []
{
    auto client = RpcClient {"eacp.tests.rpc.nobody", eacp::Time::MS {150}};

    auto error = std::string {};
    client.call<AddResult>("add", AddRequest {1, 2})
        .then([](AddResult) {}, [&](const std::string& reason) { error = reason; });

    check(runEventLoopUntil([&] { return !error.empty(); }, pumpTimeout));
};

auto tClientLeavingIsBookkept = test("Ipc/Rpc/clientLeavingIsBookkept") = []
{
    auto bridge = Miro::Bridge {};
    auto server = RpcServer {"eacp.tests.rpc.leave", bridge};

    auto left = false;
    server.onClientDisconnected = [&] { left = true; };

    auto client = std::optional<RpcClient> {};
    client.emplace("eacp.tests.rpc.leave");

    check(runEventLoopUntil([&] { return server.connectedClients() == 1; },
                            pumpTimeout));

    client.reset();

    check(runEventLoopUntil([&] { return left; }, pumpTimeout));
    check(server.connectedClients() == 0);
};
