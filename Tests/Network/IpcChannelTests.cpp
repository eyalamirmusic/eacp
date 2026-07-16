#include "Common.h"

#include <atomic>
#include <thread>

using namespace nano;
using eacp::IPC::Channel;
using eacp::IPC::ChannelServer;
namespace Proc = eacp::Processes;

namespace
{
// Mirrors IpcChannelHarness's exit codes.
constexpr auto succeeded = 0;

// Long enough that a slow CI box is not the thing being tested; a wedged
// test still fails in seconds rather than hanging on accept-forever.
constexpr auto testWait = eacp::Time::MS {10000};

bool waitForOutput(Proc::Process& harness, const std::string& text)
{
    auto deadline = eacp::Time::Deadline {eacp::Time::MS {5000}};

    while (!deadline.expired())
    {
        if (harness.output().find(text) != std::string::npos)
            return true;

        eacp::Time::sleep(eacp::Time::MS {10});
    }

    return false;
}
} // namespace

auto tNoServerThrows = test("Ipc/Channel/connectWithoutAServerThrows") = []
{
    auto threw = false;

    try
    {
        auto channel =
            Channel::connect("eacp.tests.ch.nobody", eacp::Time::MS {100});
    }
    catch (const eacp::IPC::Error&)
    {
        threw = true;
    }

    check(threw);
};

auto tEcho = test("Ipc/Channel/roundTripsBothDirections") = []
{
    auto server = ChannelServer {"eacp.tests.ch.echo"};

    auto received = std::string {};
    auto serving = std::thread(
        [&]
        {
            if (auto peer = server.accept(testWait))
            {
                received = peer->receiveLine();
                peer->send("echo: " + received + "\n");
            }
        });

    auto client = Channel::connect("eacp.tests.ch.echo");
    client.send("hello\n");
    auto reply = client.receiveLine();

    serving.join();

    check(received == "hello");
    check(reply == "echo: hello");
};

auto tAcceptTimesOut = test("Ipc/Channel/acceptTimesOutWithNobodyThere") = []
{
    auto server = ChannelServer {"eacp.tests.ch.acceptTimeout"};
    check(!server.accept(eacp::Time::MS {50}).has_value());
};

auto tConnectWaits = test("Ipc/Channel/connectWaitsForTheServerToAppear") = []
{
    auto serving = std::thread(
        []
        {
            eacp::Time::sleep(eacp::Time::MS {150});
            auto server = ChannelServer {"eacp.tests.ch.lateServer"};

            if (auto peer = server.accept(testWait))
                peer->send("ready\n");
        });

    auto client = Channel::connect("eacp.tests.ch.lateServer", testWait);
    auto line = client.receiveLine();
    serving.join();

    check(line == "ready");
};

auto tSecondServerRefused = test("Ipc/Channel/secondServerOnANameIsRefused") = []
{
    auto server = ChannelServer {"eacp.tests.ch.taken"};
    auto threw = false;

    try
    {
        auto rival = ChannelServer {"eacp.tests.ch.taken"};
    }
    catch (const eacp::IPC::Error&)
    {
        threw = true;
    }

    check(threw);
};

auto tCloseFreesTheName = test("Ipc/Channel/closedServerFreesTheName") = []
{
    auto server = ChannelServer {"eacp.tests.ch.reuse"};
    server.close();
    check(!server.isListening());

    auto threw = false;

    try
    {
        server.accept(eacp::Time::MS {10});
    }
    catch (const eacp::IPC::Error&)
    {
        threw = true;
    }

    check(threw);

    auto successor = ChannelServer {"eacp.tests.ch.reuse"};
    check(successor.isListening());
};

auto tPeerCloseEndsTheStream = test("Ipc/Channel/peerCloseEndsTheStream") = []
{
    auto server = ChannelServer {"eacp.tests.ch.eof"};

    auto sawEnd = false;
    auto serving = std::thread(
        [&]
        {
            if (auto peer = server.accept(testWait))
                sawEnd = peer->receive().empty();
        });

    {
        auto client = Channel::connect("eacp.tests.ch.eof");
    }

    serving.join();
    check(sawEnd);
};

auto tLargePayload = test("Ipc/Channel/carriesAPayloadPastTheBuffers") = []
{
    auto server = ChannelServer {"eacp.tests.ch.large"};
    auto payload = std::string(1 << 20, 'x');

    auto received = std::string {};
    auto serving = std::thread(
        [&]
        {
            if (auto peer = server.accept(testWait))
                received = peer->receiveUntil('\n');
        });

    auto client = Channel::connect("eacp.tests.ch.large");
    client.send(payload + "\n");
    serving.join();

    check(received == payload);
};

// A name is folded into a filename, so a separator must not survive to steer
// the endpoint out of its directory. Both spellings land on one endpoint,
// which is what proves the folding happened.
auto tFoldsSeparators = test("Ipc/Channel/foldsPathSeparatorsInNames") = []
{
    auto server = ChannelServer {"../eacp.tests.ch.escape"};

    auto accepted = false;
    auto serving =
        std::thread([&] { accepted = server.accept(testWait).has_value(); });

    auto client = Channel::connect(".._eacp.tests.ch.escape");
    serving.join();

    check(accepted);
    check(client.isOpen());
};

// The teardown story: a reader blocked in receive() has no timeout, so
// interrupt() from another thread must wake it with a clean end of stream.
// The interrupt is repeated the way a real teardown loop would - required
// on Windows, where a cancel only reaches a read already in flight.
auto tInterruptWakesABlockedReceive =
    test("Ipc/Channel/interruptWakesABlockedReceive") = []
{
    auto server = ChannelServer {"eacp.tests.ch.interrupt"};
    auto client = Channel::connect("eacp.tests.ch.interrupt");
    auto peer = server.accept(testWait);
    check(peer.has_value());

    auto done = std::atomic<bool> {false};
    auto received = std::string {"unset"};

    auto reading = std::thread(
        [&]
        {
            received = client.receive();
            done = true;
        });

    auto deadline = eacp::Time::Deadline {testWait};

    while (!done && !deadline.expired())
    {
        client.interrupt();
        eacp::Time::sleep(eacp::Time::MS {10});
    }

    reading.join();

    check(done);
    check(received.empty());
};

auto tEmptyNameThrows = test("Ipc/Channel/emptyNameThrows") = []
{
    auto threw = false;

    try
    {
        auto server = ChannelServer {""};
    }
    catch (const eacp::IPC::Error&)
    {
        threw = true;
    }

    check(threw);
};

auto tCrossProcessClient = test("Ipc/Channel/echoesAcrossAProcessBoundary") = []
{
    // The client launches before the server exists, so this also proves
    // connect()'s retry loop across a genuine process boundary.
    auto client = Proc::Process {EACP_IPC_CHANNEL_HARNESS,
                                 {"eacp.tests.ch.crossProc", "client", "ping"}};
    check(client.launched());

    auto server = ChannelServer {"eacp.tests.ch.crossProc"};
    auto peer = server.accept(testWait);
    check(peer.has_value());

    auto line = peer->receiveLine();
    check(line == "ping");
    peer->send("echo:" + line + "\n");

    check(client.wait() == succeeded);
};

auto tCrossProcessServer = test("Ipc/Channel/dialsAServerInAnotherProcess") = []
{
    auto harness = Proc::Process {EACP_IPC_CHANNEL_HARNESS,
                                  {"eacp.tests.ch.remoteServe", "serve"}};
    check(harness.launched());
    check(waitForOutput(harness, "listening"));

    auto client = Channel::connect("eacp.tests.ch.remoteServe");
    client.send("marco\n");
    check(client.receiveLine() == "echo:marco");
    check(harness.wait() == succeeded);
};

// The headline claim: a server that dies without cleaning up strands
// nothing. Its endpoint is a corpse, not a live rival, and claiming the
// name must sweep it aside.
auto tReclaimsAbandonedName = test("Ipc/Channel/reclaimsANameItsDeadOwnerLeft") = []
{
    auto harness = Proc::Process {EACP_IPC_CHANNEL_HARNESS,
                                  {"eacp.tests.ch.abandoned", "abandon"}};
    check(harness.launched());
    check(waitForOutput(harness, "listening"));
    harness.wait();

    auto server = ChannelServer {"eacp.tests.ch.abandoned"};
    check(server.isListening());
};
