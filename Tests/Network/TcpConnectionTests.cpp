#include "Common.h"
#include <thread>
#include <vector>

using namespace nano;
using namespace std::chrono_literals;
using eacp::TCP::Connection;
using eacp::TCP::Error;
using eacp::TCP::Listener;
using eacp::TCP::TimeoutError;

namespace
{
// Short timeouts so a wedged test fails in a couple of seconds rather than
// hanging on the generous production defaults.
eacp::TCP::Timeouts testTimeouts()
{
    return {eacp::Time::MS {2000}, eacp::Time::MS {2000}};
}

Connection dial(const Listener& listener)
{
    return Connection::connect({"127.0.0.1", listener.port()}, testTimeouts());
}

// Reads everything the peer sends until it closes - the clean way to prove a
// graceful close surfaces as an empty receive().
std::string drain(Connection& connection)
{
    auto all = std::string {};
    while (true)
    {
        auto chunk = connection.receive();
        if (chunk.empty())
            return all;
        all += chunk;
    }
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
    check(!client.isOpen());

    server.join();
};

auto tRawReceive = test("Tcp/receiveReturnsRawBytesThenEmptyOnClose") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send("no-delimiter-here");
        });

    auto client = dial(listener);
    auto got = drain(client);

    server.join();
    check(got == "no-delimiter-here");
};

auto tReceiveDrainsBuffer =
    test("Tcp/receiveDrainsBufferedOvershootBeforeTheWire") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send("head;tail-bytes");
        });

    auto client = dial(listener);
    auto head = client.receiveUntil(';');
    auto tail = drain(client); // the overshoot "tail-bytes" plus EOF

    server.join();
    check(head == "head");
    check(tail == "tail-bytes");
};

auto tCarriageReturnTrim = test("Tcp/receiveLineStripsTrailingCarriageReturn") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send("crlf-terminated\r\n");
        });

    auto client = dial(listener);
    auto line = client.receiveLine();

    server.join();
    check(line == "crlf-terminated");
};

auto tEmptyLines = test("Tcp/receiveLineHandlesEmptyLines") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send("a\n\nb\n");
        });

    auto client = dial(listener);
    auto first = client.receiveLine();
    auto second = client.receiveLine();
    auto third = client.receiveLine();

    server.join();
    check(first == "a");
    check(second.empty());
    check(third == "b");
};

auto tManyConnections = test("Tcp/listenerServesManyConnections") = []
{
    constexpr auto clients = 4;
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            for (auto i = 0; i < clients; ++i)
            {
                auto peer = listener.accept();
                peer.send(peer.receiveLine() + "-ack\n");
            }
        });

    auto connections = std::vector<Connection> {};
    for (auto i = 0; i < clients; ++i)
    {
        connections.push_back(dial(listener));
        connections.back().send("c" + std::to_string(i) + "\n");
    }

    auto ok = true;
    for (auto i = 0; i < clients; ++i)
        ok = ok && connections[i].receiveLine() == "c" + std::to_string(i) + "-ack";

    server.join();
    check(ok);
};

auto tMoveConnection = test("Tcp/movingAConnectionTransfersTheSocket") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send(peer.receiveLine() + "\n");
        });

    auto client = dial(listener);
    auto moved = std::move(client);

    check(!client.isOpen()); // NOLINT - intentionally inspecting moved-from
    check(moved.isOpen());

    moved.send("through-the-moved-handle\n");
    auto reply = moved.receiveLine();

    server.join();
    check(reply == "through-the-moved-handle");
};

auto tMoveListener = test("Tcp/movingAListenerKeepsItServing") = []
{
    auto original = Listener::bind(0, testTimeouts());
    auto port = original.port();
    auto listener = std::move(original);

    check(!original.isListening()); // NOLINT - moved-from
    check(listener.isListening());
    check(listener.port() == port);

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send("served\n");
        });

    auto client = dial(listener);
    auto line = client.receiveLine();

    server.join();
    check(line == "served");
};

auto tAcceptTimeout = test("Tcp/acceptTimesOutWhenNoClientConnects") = []
{
    auto listener = Listener::bind(0, {eacp::Time::MS {200}, eacp::Time::MS {2000}});

    auto threw = false;
    try
    {
        listener.accept();
    }
    catch (const Error&)
    {
        threw = true;
    }

    check(threw);
};

auto tAcceptAfterClose = test("Tcp/acceptThrowsAfterTheListenerIsClosed") = []
{
    auto listener = Listener::bind(0, testTimeouts());
    check(listener.isListening());

    listener.close();
    check(!listener.isListening());

    auto threw = false;
    try
    {
        listener.accept();
    }
    catch (const Error&)
    {
        threw = true;
    }

    check(threw);
};

auto tSendAfterClose = test("Tcp/sendThrowsOnAClosedConnection") = []
{
    auto listener = Listener::bind(0, testTimeouts());
    auto server = std::thread([&] { auto peer = listener.accept(); });

    auto client = dial(listener);
    client.close();

    auto threw = false;
    try
    {
        client.send("anyone there?\n");
    }
    catch (const Error&)
    {
        threw = true;
    }

    server.join();
    check(threw);
};

auto tServerSideLargeUpload = test("Tcp/serverReassemblesALargeUpload") = []
{
    auto big = std::string(200000, 'y');
    auto listener = Listener::bind(0, testTimeouts());

    auto received = std::string {};
    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            received = peer.receiveLine();
        });

    auto client = dial(listener);
    client.send(big + "\n");

    server.join();
    check(received.size() == big.size());
    check(received == big);
};

// ---- hostile / adversarial cases ----------------------------------------

auto tSendToHungUpPeer = test("Tcp/sendToAHungUpPeerThrowsRatherThanCrashing") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    // Server accepts and drops the connection straight away.
    auto server = std::thread([&] { auto peer = listener.accept(); });

    auto client = dial(listener);
    server.join();
    std::this_thread::sleep_for(50ms); // let the reset reach us

    // Writing to a peer that has gone away must surface as an Error, not a
    // SIGPIPE that takes the whole process down. Keep pushing until the OS
    // notices the broken pipe.
    auto threw = false;
    try
    {
        for (auto i = 0; i < 5000 && !threw; ++i)
            client.send(std::string(4096, 'x'));
    }
    catch (const Error&)
    {
        threw = true;
    }

    check(threw);
};

auto tFragmentedReply = test("Tcp/reassemblesAReplyArrivingInTinyFragments") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            auto message = std::string {"drip-fed-message"};
            for (auto c: message)
            {
                peer.send(std::string(1, c));
                std::this_thread::sleep_for(2ms);
            }
            peer.send("\n");
        });

    auto client = dial(listener);
    auto line = client.receiveLine();

    server.join();
    check(line == "drip-fed-message");
};

auto tSplitDelimiter = test("Tcp/receiveUntilHandlesADelimiterSplitAcrossReads") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send("alpha-");
            std::this_thread::sleep_for(20ms);
            peer.send("beta\nsecond-line\n");
        });

    auto client = dial(listener);
    auto first = client.receiveLine(); // delimiter arrives in the 2nd read
    auto second = client.receiveLine(); // came in as overshoot on the 1st

    server.join();
    check(first == "alpha-beta");
    check(second == "second-line");
};

auto tIdleReceiveTimesOut =
    test("Tcp/receiveTimesOutWithTimeoutErrorWhenPeerIsIdle") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    // Server holds the connection open but sends nothing.
    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            std::this_thread::sleep_for(500ms);
        });

    // Short io timeout so the idle read trips quickly.
    auto client = Connection::connect({"127.0.0.1", listener.port()},
                                      {eacp::Time::MS {2000}, eacp::Time::MS {200}});

    auto timedOut = false;
    try
    {
        client.receive();
    }
    catch (const TimeoutError&)
    {
        timedOut = true;
    }

    server.join();
    check(timedOut);
};

auto tBinaryPayload = test("Tcp/binaryPayloadWithNulsAndHighBytesRoundTrips") = []
{
    // Every byte except the framing ones - NULs, 0xFF, the lot.
    auto payload = std::string {};
    for (auto b = 0; b < 256; ++b)
        if (b != '\n' && b != '\r')
            payload.push_back((char) b);

    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send(payload + "\n");
        });

    auto client = dial(listener);
    auto line = client.receiveLine();

    server.join();
    check(line.size() == payload.size());
    check(line == payload);
};

auto tPartialThenClose = test("Tcp/partialDataThenCloseStillThrows") = []
{
    auto listener = Listener::bind(0, testTimeouts());

    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            peer.send("incomplete-no-delimiter"); // never a newline, then close
        });

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

auto tDoubleClose = test("Tcp/doubleCloseIsHarmless") = []
{
    auto listener = Listener::bind(0, testTimeouts());
    auto server = std::thread([&] { auto peer = listener.accept(); });

    auto client = dial(listener);
    client.close();
    client.close(); // must be a safe no-op

    check(!client.isOpen());
    server.join();
};

auto tUnreachableHost = test("Tcp/connectToAnUnreachableHostFails") = []
{
    // 192.0.2.0/24 is reserved (RFC 5737) and routes nowhere, so this either
    // times out or is rejected - both surface as Error within the budget.
    auto threw = false;
    try
    {
        auto client = Connection::connect(
            {"192.0.2.1", 80}, {eacp::Time::MS {800}, eacp::Time::MS {800}});
    }
    catch (const Error&)
    {
        threw = true;
    }

    check(threw);
};

auto tMultiMegabyte = test("Tcp/streamsAMultiMegabytePayload") = []
{
    auto big = std::string(2 * 1024 * 1024, 'z'); // 2 MB through the send loop
    auto listener = Listener::bind(0, testTimeouts());

    auto received = std::string {};
    auto server = std::thread(
        [&]
        {
            auto peer = listener.accept();
            received = peer.receiveLine();
        });

    auto client = dial(listener);
    client.send(big + "\n");

    server.join();
    check(received.size() == big.size());
    check(received == big);
};
