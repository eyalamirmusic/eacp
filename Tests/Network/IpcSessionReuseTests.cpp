#include <eacp/Network/IPC/Messenger.h>
#include <eacp/Network/IPCRpc/RpcClient.h>
#include <eacp/Network/IPCRpc/RpcServer.h>

#include <NanoTest/NanoTest.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Its own binary because it replaces the global allocator: to prove a reply
// can't follow a departed session's address, the departed session's memory
// has to be handed to the next session on purpose rather than by luck.

namespace
{
constexpr auto quarantineCapacity = 8;

struct BlockSize
{
    std::size_t bytes = 0;

    [[nodiscard]] bool isSessionSized() const
    {
        return bytes == sizeof(eacp::IPC::Messenger);
    }
};

struct Block
{
    void* address = nullptr;
};

struct SessionAddressRecycler
{
    std::atomic<bool> isCapturing = false;
    std::atomic<bool> isReusing = false;
    std::atomic<std::thread::id> mainThread {};
    std::array<std::atomic<void*>, quarantineCapacity> captured {};
    std::atomic<void*> quarantined = nullptr;
    std::atomic<int> reusedCount = 0;

    void captureIfSession(Block block, BlockSize size)
    {
        if (!isCapturing || !size.isSessionSized())
            return;

        if (std::this_thread::get_id() == mainThread.load())
            return;

        for (auto& slot: captured)
        {
            auto* expected = static_cast<void*>(nullptr);

            if (slot.compare_exchange_strong(expected, block.address))
                return;
        }
    }

    bool quarantineIfCaptured(Block block)
    {
        for (auto& slot: captured)
        {
            auto* expected = block.address;

            if (block.address != nullptr
                && slot.compare_exchange_strong(expected, nullptr))
            {
                quarantined = block.address;
                return true;
            }
        }

        return false;
    }

    void* takeQuarantinedFor(BlockSize size)
    {
        if (!isReusing || !size.isSessionSized())
            return nullptr;

        auto* block = quarantined.exchange(nullptr);

        if (block != nullptr)
            ++reusedCount;

        return block;
    }
};

SessionAddressRecycler& recycler()
{
    static auto instance = SessionAddressRecycler {};
    return instance;
}
} // namespace

void* operator new(std::size_t size)
{
    if (auto* reused = recycler().takeQuarantinedFor(BlockSize {size}))
        return reused;

    auto* block = std::malloc(size == 0 ? 1 : size);

    if (block == nullptr)
        throw std::bad_alloc {};

    recycler().captureIfSession(Block {block}, BlockSize {size});
    return block;
}

void operator delete(void* block) noexcept
{
    if (!recycler().quarantineIfCaptured(Block {block}))
        std::free(block);
}

void operator delete(void* block, std::size_t) noexcept
{
    operator delete(block);
}

using namespace nano;
using eacp::IPC::RpcClient;
using eacp::IPC::RpcServer;
using eacp::Threads::runEventLoopUntil;

namespace
{
constexpr auto reusePumpTimeout = eacp::Time::MS {5000};
constexpr auto strayReplyWait = eacp::Time::MS {200};
constexpr auto departedClientsAnswer = 111;
constexpr auto newClientsAnswer = 222;

struct HeldRequest
{
    int value = 0;

    MIRO_REFLECT(value)
};

struct HeldResult
{
    int value = 0;

    MIRO_REFLECT(value)
};

struct HeldCalls
{
    std::vector<Miro::Completer<HeldResult>> waiting;
    std::size_t answeredCount = 0;

    void hold(const HeldRequest&, Miro::Completer<HeldResult> done)
    {
        waiting.push_back(std::move(done));
    }

    void answerNext(HeldResult result) { waiting[answeredCount++].resolve(result); }
};

void pumpBriefly()
{
    runEventLoopUntil([] { return false; }, strayReplyWait);
}
} // namespace

auto tReplyNeverReachesANewSessionAtTheSameAddress =
    test("Ipc/Rpc/aLateReplyNeverReachesANewSessionAtTheDepartedOnesAddress") = []
{
    recycler().mainThread = std::this_thread::get_id();

    auto held = HeldCalls {};
    auto bridge = Miro::Bridge {};
    bridge.onAsync<HeldRequest, HeldResult>(
        "hold",
        [&held](const HeldRequest& request, Miro::Completer<HeldResult> done)
        { held.hold(request, std::move(done)); });

    const auto name = std::string {"eacp.tests.rpc.sessionReuse"};
    auto server = RpcServer {name, bridge};

    recycler().isCapturing = true;
    auto departing = std::optional<RpcClient> {};
    departing.emplace(name);
    departing->call<HeldResult>("hold", HeldRequest {1})
        .then([](HeldResult) {}, [](const std::string&) {});
    check(runEventLoopUntil([&held] { return held.waiting.size() == 1; },
                            reusePumpTimeout));
    recycler().isCapturing = false;

    departing.reset();
    check(runEventLoopUntil([&server] { return server.connectedClients() == 0; },
                            reusePumpTimeout));

    auto sweeper = RpcClient {name};
    check(runEventLoopUntil([&sweeper] { return sweeper.isConnected(); },
                            reusePumpTimeout));
    pumpBriefly();

    recycler().isReusing = true;
    auto arriving = RpcClient {name};
    auto arrivingAnswer = 0;
    arriving.call<HeldResult>("hold", HeldRequest {2})
        .then([&arrivingAnswer](HeldResult result)
              { arrivingAnswer = result.value; },
              [](const std::string&) {});
    check(runEventLoopUntil([&held] { return held.waiting.size() == 2; },
                            reusePumpTimeout));
    recycler().isReusing = false;

    check(recycler().reusedCount == 1);

    held.answerNext({departedClientsAnswer});
    pumpBriefly();
    check(arrivingAnswer == 0);

    held.answerNext({newClientsAnswer});
    check(runEventLoopUntil([&arrivingAnswer] { return arrivingAnswer != 0; },
                            reusePumpTimeout));
    check(arrivingAnswer == newClientsAnswer);
};
