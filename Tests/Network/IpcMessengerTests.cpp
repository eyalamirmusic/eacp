#include "Common.h"

#include <optional>

using namespace nano;
using eacp::IPC::MessageServer;
using eacp::IPC::Messenger;
using eacp::Threads::runEventLoopUntil;

namespace
{
constexpr auto pumpTimeout = eacp::Time::MS {10000};
}

auto tRoundTrip = test("Ipc/Messenger/roundTripsBothDirections") = []
{
    auto server = MessageServer {"eacp.tests.msgr.echo"};

    auto serverGot = std::string {};
    server.onClient = [&](Messenger& session)
    {
        session.onMessage = [&](const std::string& message)
        {
            serverGot = message;
            session.send("pong:" + message);
        };
    };

    auto connected = false;
    auto reply = std::string {};

    auto client = Messenger {"eacp.tests.msgr.echo"};
    client.onConnected = [&]
    {
        connected = true;
        client.send("ping");
    };
    client.onMessage = [&](const std::string& message) { reply = message; };

    check(runEventLoopUntil([&] { return !reply.empty(); }, pumpTimeout));
    check(connected);
    check(serverGot == "ping");
    check(reply == "pong:ping");
};

// Length-prefixed framing is the difference between "messages" and "lines":
// a payload full of delimiters and NULs - and bigger than any socket
// buffer - must arrive as the one message it left as.
auto tBinaryPayload = test("Ipc/Messenger/carriesArbitraryBytesIntact") = []
{
    auto payload = std::string {"lines\nand\r\nmore"};
    payload += '\0';
    payload += std::string(1 << 20, 'x');

    auto server = MessageServer {"eacp.tests.msgr.binary"};

    auto arrived = std::string {};
    server.onClient = [&](Messenger& session)
    { session.onMessage = [&](const std::string& message) { arrived = message; }; };

    auto client = Messenger {"eacp.tests.msgr.binary"};
    client.onConnected = [&] { client.send(payload); };

    check(runEventLoopUntil([&] { return !arrived.empty(); }, pumpTimeout));
    check(arrived == payload);
};

auto tClientLeavingNotifiesServer =
    test("Ipc/Messenger/clientLeavingNotifiesTheServer") = []
{
    auto server = MessageServer {"eacp.tests.msgr.leave"};

    auto arrived = false;
    auto gone = false;
    server.onClient = [&](Messenger& session)
    {
        arrived = true;
        session.onDisconnected = [&] { gone = true; };
    };

    {
        auto client = std::optional<Messenger> {};
        client.emplace("eacp.tests.msgr.leave");
        check(runEventLoopUntil([&] { return arrived; }, pumpTimeout));
        client.reset();
    }

    check(runEventLoopUntil([&] { return gone; }, pumpTimeout));
};

auto tServerDyingNotifiesClient =
    test("Ipc/Messenger/serverDyingNotifiesTheClient") = []
{
    auto server = std::optional<MessageServer> {};
    server.emplace("eacp.tests.msgr.serverGone");

    auto connected = false;
    auto gone = false;

    auto client = Messenger {"eacp.tests.msgr.serverGone"};
    client.onConnected = [&] { connected = true; };
    client.onDisconnected = [&] { gone = true; };

    check(runEventLoopUntil([&] { return connected; }, pumpTimeout));

    server.reset();

    check(runEventLoopUntil([&] { return gone; }, pumpTimeout));
    check(!client.isConnected());
};

auto tDialFailureNotifies = test("Ipc/Messenger/failedDialNotifiesDisconnected") = []
{
    auto gone = false;

    auto client = Messenger {"eacp.tests.msgr.nobody", eacp::Time::MS {150}};
    client.onDisconnected = [&] { gone = true; };

    check(runEventLoopUntil([&] { return gone; }, pumpTimeout));
    check(!client.isConnected());
};

auto tNameTaken = test("Ipc/Messenger/secondServerOnANameIsRefused") = []
{
    auto server = MessageServer {"eacp.tests.msgr.taken"};
    auto threw = false;

    try
    {
        auto rival = MessageServer {"eacp.tests.msgr.taken"};
    }
    catch (const eacp::IPC::Error&)
    {
        threw = true;
    }

    check(threw);
};

// Sessions are served in turn: a finished one is swept when the next client
// arrives, and each gets its own working conversation.
auto tServesClientsInTurn = test("Ipc/Messenger/servesClientsInTurn") = []
{
    auto server = MessageServer {"eacp.tests.msgr.turns"};

    auto greeted = 0;
    server.onClient = [&](Messenger& session)
    {
        session.onMessage = [&](const std::string& message)
        { session.send("hello " + message); };
    };

    for (auto round = 0; round < 2; ++round)
    {
        auto reply = std::string {};

        auto client = std::optional<Messenger> {};
        client.emplace("eacp.tests.msgr.turns");
        client->onConnected = [&] { client->send(std::to_string(round)); };
        client->onMessage = [&](const std::string& message)
        {
            reply = message;
            ++greeted;
        };

        check(runEventLoopUntil([&] { return !reply.empty(); }, pumpTimeout));
        check(reply == "hello " + std::to_string(round));
    }

    check(greeted == 2);
};
