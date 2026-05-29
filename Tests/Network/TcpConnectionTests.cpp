#include <eacp/Network/TCP/Connection.h>
#include <eacp/Network/TCP/Listener.h>

#include <NanoTest/NanoTest.h>

#include <chrono>
#include <string>
#include <thread>

using namespace nano;
using namespace std::chrono_literals;
using eacp::TCP::Connection;
using eacp::TCP::Error;
using eacp::TCP::Listener;

namespace
{
// Short timeouts so a wedged test fails in a couple of seconds rather than
// hanging on the generous production defaults.
eacp::TCP::Timeouts testTimeouts()
{
    return {2000ms, 2000ms};
}

Connection dial(const Listener& listener)
{
    return Connection::connect({"127.0.0.1", listener.port()}, testTimeouts());
}
} // namespace

auto tRoundTripLine = test("Tcp/roundTripsASingleLine") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto received = std::string {};
    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            received = peer.receiveLine();
            peer.send("echo: " + received + "\n");
        });

    auto client = dial(listener);
    client.send("hello\n");
    auto reply = client.receiveLine();

    server.join();

    check(received == "hello");
    check(reply == "echo: hello");
};

auto tMultipleLines = test("Tcp/streamsManyLinesOverOneConnection") = []
{
    constexpr auto lines = 5;
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            for (auto i = 0; i < lines; ++i)
                peer.send(peer.receiveLine() + "!\n");
        });

    auto client = dial(listener);
    auto ok = true;
    for (auto i = 0; i < lines; ++i)
    {
        client.send("line" + std::to_string(i) + "\n");
        ok = ok && client.receiveLine() == "line" + std::to_string(i) + "!";
    }

    server.join();
    check(ok);
};

auto tLargePayload = test("Tcp/reassemblesAPayloadLargerThanOneRead") = []
{
    // Far bigger than the 4096-byte receive chunk, so receiveLine() has to
    // loop and stitch several reads back together.
    auto big = std::string(200000, 'x');
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send(big + "\n");
        });

    auto client = dial(listener);
    auto line = client.receiveLine();

    server.join();
    check(line.size() == big.size());
    check(line == big);
};

auto tCustomDelimiter = test("Tcp/receiveUntilHonoursACustomDelimiter") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send("alpha;beta;");
        });

    auto client = dial(listener);
    auto first = client.receiveUntil(';');
    auto second = client.receiveUntil(';');

    server.join();
    check(first == "alpha");
    check(second == "beta");
};

auto tPeerAddress = test("Tcp/acceptedConnectionKnowsItsPeer") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto peerHost = std::string {};
    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peerHost = peer.address().host;
        });

    auto client = dial(listener);
    server.join();

    check(peerHost == "127.0.0.1");
};

auto tEphemeralPort = test("Tcp/bindZeroAssignsAnEphemeralPort") = []
{
    auto listener = Listener::bind(0, testTimeouts());
    check(listener.isListening());
    check(listener.port() != 0);
};

auto tPeerCloseThrows = test("Tcp/receiveThrowsWhenPeerClosesBeforeDelimiter") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    // Server accepts and immediately drops the connection.
    auto server = std::thread([&] { auto peer = listener.accept(); });

    auto client = dial(listener);

    auto threw = false;
    try
    {
        client.receiveLine();
    }
    catch (const Error&)
    {
        threw = true;
    }

    server.join();
    check(threw);
};

auto tConnectRefusedThrows = test("Tcp/connectThrowsWhenNothingIsListening") = []
{
    // Bind to claim an ephemeral port, then free it so the connect is refused.
    auto port = std::uint16_t {0};
    {
        auto listener = Listener::bind(0, testTimeouts());
        port = listener.port();
    }

    auto threw = false;
    try
    {
        auto client = Connection::connect({"127.0.0.1", port}, testTimeouts());
    }
    catch (const Error&)
    {
        threw = true;
    }

    check(threw);
};

auto tCloseEndsConnection = test("Tcp/closeMakesAConnectionNotOpen") = []
{
    auto listener = Listener::bind(0, testTimeouts());
    auto server = std::thread([&] { auto peer = listener.accept(); });

    auto client = dial(listener);
    check(client.isOpen());
    client.close();
    check(! client.isOpen());

    server.join();
};
