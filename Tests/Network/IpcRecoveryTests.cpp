#include <eacp/Network/IPC/Messenger.h>
#include <eacp/Network/IPCRpc/RpcClient.h>
#include <eacp/Network/IPCRpc/RpcServer.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <NanoTest/NanoTest.h>

using namespace nano;
using eacp::IPC::Channel;
using eacp::IPC::MessageServer;
using eacp::IPC::Messenger;
using eacp::IPC::RpcClient;
using eacp::IPC::RpcServer;
using eacp::Threads::runEventLoopUntil;

namespace
{
constexpr auto recoveryPumpTimeout = eacp::Time::MS {5000};
constexpr auto frameHeaderBytes = 4;
constexpr auto announcedPayloadBytes = std::uint32_t {100};
constexpr auto deliveredPayloadBytes = 10;
constexpr auto implausibleLength = std::uint32_t {0xffffffff};
constexpr auto concurrentClientCount = 16;
constexpr auto overflowMessageBytes = std::size_t {1} << 20;
constexpr auto overflowMessageCount = 80;
constexpr auto floodSendingLimit = eacp::Time::MS {500};
constexpr auto floodPollInterval = eacp::Time::MS {5};

struct EchoRequest
{
    int value = 0;

    MIRO_REFLECT(value)
};

struct EchoResult
{
    int value = 0;

    MIRO_REFLECT(value)
};

class EchoApi
{
public:
    void reflect(Miro::ApiReflector& r) { r.command(&EchoApi::echo, "echo"); }

    EchoResult echo(const EchoRequest& request) const { return {request.value}; }
};

std::string encodeFrameHeader(std::uint32_t length)
{
    auto header = std::string(frameHeaderBytes, '\0');

    for (auto index = 0; index < frameHeaderBytes; ++index)
        header[(std::size_t) index] = (char) ((length >> (index * 8)) & 0xff);

    return header;
}

struct CountingServer
{
    MessageServer server;
    int sessionsSeen = 0;
    int sessionsGone = 0;
    std::string lastMessage;

    explicit CountingServer(const std::string& name)
        : server(name)
    {
        server.onClient = [this](Messenger& session)
        {
            ++sessionsSeen;
            session.onDisconnected = [this] { ++sessionsGone; };
            session.onMessage = [this, &session](const std::string& message)
            {
                lastMessage = message;
                session.send("echo:" + message);
            };
        };
    }
};

bool isFloodSentWithinLimit(Messenger& session, Channel& slowReader)
{
    auto isFinished = std::atomic<bool> {false};
    auto sender = std::thread(
        [&session, &isFinished]
        {
            auto payload = std::string(overflowMessageBytes, 'x');

            for (auto index = 0; index < overflowMessageCount; ++index)
                session.send(payload);

            isFinished = true;
        });

    auto deadline = eacp::Time::Deadline {floodSendingLimit};

    while (!isFinished && !deadline.expired())
        eacp::Time::sleep(floodPollInterval);

    auto isFinishedInTime = isFinished.load();

    if (!isFinishedInTime)
        slowReader.close();

    sender.join();
    return isFinishedInTime;
}

bool isServedAfterwards(CountingServer& counting, const std::string& name)
{
    auto reply = std::string {};
    auto client = Messenger {name};
    client.onConnected = [&client] { client.send("again"); };
    client.onMessage = [&reply](const std::string& message) { reply = message; };

    return runEventLoopUntil([&reply] { return !reply.empty(); },
                             recoveryPumpTimeout)
           && reply == "echo:again" && counting.lastMessage == "again";
}
} // namespace

auto tPeerDyingMidMessage =
    test("Ipc/Recovery/aPeerDyingMidMessageEndsOnlyItsSession") = []
{
    const auto name = std::string {"eacp.tests.recovery.midMessage"};
    auto counting = CountingServer {name};

    {
        auto raw = Channel::connect(name);
        raw.send(encodeFrameHeader(announcedPayloadBytes));
        raw.send(std::string((std::size_t) deliveredPayloadBytes, 'x'));
    }

    check(runEventLoopUntil([&counting] { return counting.sessionsGone == 1; },
                            recoveryPumpTimeout));
    check(counting.lastMessage.empty());
    check(isServedAfterwards(counting, name));
};

auto tGarbageFrame = test("Ipc/Recovery/anImplausibleFrameEndsOnlyItsSession") = []
{
    const auto name = std::string {"eacp.tests.recovery.garbage"};
    auto counting = CountingServer {name};

    auto raw = Channel::connect(name);
    raw.send(encodeFrameHeader(implausibleLength));

    check(runEventLoopUntil([&counting] { return counting.sessionsGone == 1; },
                            recoveryPumpTimeout));
    check(raw.receive().empty());
    check(isServedAfterwards(counting, name));
};

auto tConcurrentClients =
    test("Ipc/Recovery/manyConcurrentClientsEachGetTheirOwnAnswer") = []
{
    auto api = EchoApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    const auto name = std::string {"eacp.tests.recovery.concurrent"};
    auto server = RpcServer {name, bridge};

    auto clients = std::vector<std::unique_ptr<RpcClient>> {};
    auto answers = std::vector<int>(concurrentClientCount, -1);

    for (auto index = 0; index < concurrentClientCount; ++index)
    {
        clients.push_back(std::make_unique<RpcClient>(name));
        clients.back()
            ->call<EchoResult>("echo", EchoRequest {index})
            .then([&answers, index](EchoResult result)
                  { answers[(std::size_t) index] = result.value; },
                  [](const std::string&) {});
    }

    auto isEveryAnswerIn = [&answers]
    {
        for (auto answer: answers)
        {
            if (answer < 0)
                return false;
        }

        return true;
    };

    check(runEventLoopUntil(isEveryAnswerIn, recoveryPumpTimeout));

    for (auto index = 0; index < concurrentClientCount; ++index)
        check(answers[(std::size_t) index] == index);
};

auto tStuckReaderRecovers =
    test("Ipc/Recovery/aDisconnectedSlowReaderIsServedWhenItDialsAgain") = []
{
    const auto name = std::string {"eacp.tests.recovery.slowReader"};
    auto counting = CountingServer {name};
    auto session = std::optional<Messenger*> {};
    counting.server.onClient = [&counting, &session](Messenger& arrived)
    {
        ++counting.sessionsSeen;
        session = &arrived;
        arrived.onDisconnected = [&counting] { ++counting.sessionsGone; };
        arrived.onMessage = [&counting, &arrived](const std::string& message)
        {
            counting.lastMessage = message;
            arrived.send("echo:" + message);
        };
    };

    auto slowReader = Channel::connect(name);
    check(runEventLoopUntil([&session] { return session.has_value(); },
                            recoveryPumpTimeout));

    check(isFloodSentWithinLimit(**session, slowReader));
    check(runEventLoopUntil([&counting] { return counting.sessionsGone == 1; },
                            recoveryPumpTimeout));
    check(isServedAfterwards(counting, name));
};

auto tRpcClientSeesHubRestart =
    test("Ipc/Recovery/aRestartedServerAnswersAFreshlyDialedClient") = []
{
    auto api = EchoApi {};
    auto bridge = Miro::Bridge {};
    bridge.use(api);

    const auto name = std::string {"eacp.tests.recovery.restart"};
    auto server = std::optional<RpcServer> {};
    server.emplace(name, bridge);

    auto client = std::optional<RpcClient> {};
    client.emplace(name);
    auto isDropped = false;
    client->onDisconnected = [&isDropped] { isDropped = true; };

    auto inFlightFailed = false;
    check(runEventLoopUntil([&client] { return client->isConnected(); },
                            recoveryPumpTimeout));

    server.reset();
    client->call<EchoResult>("echo", EchoRequest {1})
        .then([](EchoResult) {},
              [&inFlightFailed](const std::string&) { inFlightFailed = true; });

    check(runEventLoopUntil([&isDropped, &inFlightFailed]
                            { return isDropped && inFlightFailed; },
                            recoveryPumpTimeout));

    server.emplace(name, bridge);
    client.emplace(name);

    auto answer = 0;
    client->call<EchoResult>("echo", EchoRequest {7})
        .then([&answer](EchoResult result) { answer = result.value; },
              [](const std::string&) {});

    check(runEventLoopUntil([&answer] { return answer == 7; }, recoveryPumpTimeout));
};

namespace
{
constexpr auto stuckSessionCount = 3;
constexpr auto stuckSessionBacklogMessages = 4;
constexpr auto serverShutdownLimit = eacp::Time::MS {900};
} // namespace

auto tServerShutsDownPastStuckSessions =
    test("Ipc/Recovery/aServerWithStuckSessionsShutsDownPromptly") = []
{
    const auto name = std::string {"eacp.tests.recovery.stuckShutdown"};
    auto server = std::optional<MessageServer> {};
    server.emplace(name);

    auto sessions = std::vector<Messenger*> {};
    server->onClient = [&sessions](Messenger& arrived) { sessions.push_back(&arrived); };

    auto stuckReaders = std::vector<Channel> {};

    for (auto index = 0; index < stuckSessionCount; ++index)
        stuckReaders.push_back(Channel::connect(name));

    check(runEventLoopUntil([&sessions]
                            { return sessions.size() == stuckSessionCount; },
                            recoveryPumpTimeout));

    auto payload = std::string(overflowMessageBytes, 'x');

    for (auto* session: sessions)
    {
        for (auto index = 0; index < stuckSessionBacklogMessages; ++index)
            session->send(payload);
    }

    auto deadline = eacp::Time::Deadline {serverShutdownLimit};
    server.reset();

    check(!deadline.expired());
};
