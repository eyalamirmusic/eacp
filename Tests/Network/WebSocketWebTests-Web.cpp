#include "WebCommon.h"

using namespace nano;
using eacp::WebSocket::CloseStatus;
using eacp::WebSocket::Connection;
using eacp::WebSocket::Message;
using eacp::WebSocket::MessageType;
using eacp::WebSocket::Options;
using eacp::WebSocket::State;

namespace WebSocketWebTests
{
using namespace WebTests;

// The close timeout is a pending callAfter, which keeps node's process alive
// after the cases until it fires, so the suite keeps it short.
Options quickOptions()
{
    auto options = Options {};
    options.closeTimeout = eacp::Time::MS {1000};
    return options;
}

struct Recorder
{
    eacp::WebSocket::Callbacks callbacks()
    {
        auto result = eacp::WebSocket::Callbacks {};
        result.onOpen = [this](const std::string& chosen)
        {
            opened = true;
            protocol = chosen;
        };
        result.onMessage = [this](const Message& message) { messages.add(message); };
        result.onClose = [this](const CloseStatus& status) { closed = status; };
        result.onError = [this](const std::string& message) { error = message; };
        return result;
    }

    bool waitForOpen()
    {
        return eacp::Threads::runEventLoopUntil([this] { return opened || closed; },
                                                fetchTimeout);
    }

    bool waitForMessages(int count)
    {
        return eacp::Threads::runEventLoopUntil(
            [this, count] { return messages.size() >= count || closed; },
            fetchTimeout);
    }

    bool waitForClose() { return waitFor(closed); }

    bool opened = false;
    std::string protocol;
    eacp::Vector<Message> messages;
    std::optional<CloseStatus> closed;
    std::string error;
};

auto tSupported =
    test("WebWebSocket/isSupported") = [] { check(Connection::isSupported()); };

auto tEcho = test("WebWebSocket/echoesTextAndBinary") = []
{
    auto recorder = Recorder {};
    auto options = quickOptions();
    options.protocols = {"eacp-test", "other"};
    auto connection =
        Connection {webSocketUrl("/ws"), recorder.callbacks(), options};

    check(recorder.waitForOpen());
    check(recorder.opened);
    check(recorder.protocol == "eacp-test");
    check(connection.state() == State::open);

    auto binary = pattern(0, 70000);
    connection.send("hello");
    connection.sendBinary(binary);
    check(recorder.waitForMessages(2));
    check(recorder.messages.size() == 2);
    check(recorder.messages[0].data == "hello");
    check(recorder.messages[0].type == MessageType::text);
    check(recorder.messages[1].data == binary);
    check(recorder.messages[1].type == MessageType::binary);

    connection.close(1000, "done");
    check(recorder.waitForClose());
    check(recorder.closed->code == 1000);
    check(recorder.closed->reason == "done");
    check(recorder.error.empty());
};

auto tServerClose = test("WebWebSocket/theServerClosingIsReported") = []
{
    auto recorder = Recorder {};
    auto connection =
        Connection {webSocketUrl("/ws"), recorder.callbacks(), quickOptions()};
    check(recorder.waitForOpen());

    connection.send("close 4001 bye");
    check(recorder.waitForClose());
    check(recorder.closed->code == 4001);
    check(recorder.closed->reason == "bye");
    check(connection.state() == State::closed);
};

// A browser refuses any other code from a script; the server echoes the one
// it received.
auto tCloseCode = test("WebWebSocket/aCodeABrowserRefusesIsSentAs1000") = []
{
    for (auto [asked, sent]: {std::pair {1001, 1000}, std::pair {4002, 4002}})
    {
        auto recorder = Recorder {};
        auto connection =
            Connection {webSocketUrl("/ws"), recorder.callbacks(), quickOptions()};
        check(recorder.waitForOpen());

        connection.close(asked, "why");
        check(recorder.waitForClose());
        check(recorder.closed->code == sent);
    }
};

auto tHeaders = test("WebWebSocket/headersFailAtConstruction") = []
{
    auto recorder = Recorder {};
    auto options = quickOptions();
    options.headers["Authorization"] = "Bearer secret";
    auto connection =
        Connection {webSocketUrl("/ws"), recorder.callbacks(), options};

    check(recorder.waitForClose());
    check(recorder.error.find("headers") != std::string::npos);
    check(recorder.closed->code == 1006);
};

auto tRejected = test("WebWebSocket/aRefusedHandshakeFails") = []
{
    auto recorder = Recorder {};
    auto connection = Connection {
        webSocketUrl("/ws-reject"), recorder.callbacks(), quickOptions()};

    check(recorder.waitForClose());
    check(!recorder.opened);
    check(!recorder.error.empty());
    check(recorder.closed->code == 1006);
};

auto tConnectTimeout = test("WebWebSocket/aHandshakeNobodyAnswersTimesOut") = []
{
    auto recorder = Recorder {};
    auto options = quickOptions();
    options.connectTimeout = eacp::Time::MS {300};
    auto start = Clock::now();
    auto connection =
        Connection {webSocketUrl("/ws-hang"), recorder.callbacks(), options};

    check(recorder.waitForClose());
    check(recorder.error == "connection timed out");
    check(recorder.closed->code == 1006);
    check(millisecondsSince(start) < 3000);
};

auto tTooBig = test("WebWebSocket/aMessageOverTheLimitFails") = []
{
    auto recorder = Recorder {};
    auto options = quickOptions();
    options.maxMessageSize = 1000;
    auto connection =
        Connection {webSocketUrl("/ws"), recorder.callbacks(), options};
    check(recorder.waitForOpen());

    connection.send("send 1000");
    check(recorder.waitForMessages(1));
    check(recorder.messages.size() == 1);

    connection.send("send 1001");
    check(recorder.waitForClose());
    check(recorder.error == "message exceeds maxMessageSize");
    check(recorder.closed->code == 1006);
};

auto tCloseTimeout = test("WebWebSocket/aCloseNobodyAnswersStillEnds") = []
{
    auto recorder = Recorder {};
    auto options = quickOptions();
    options.closeTimeout = eacp::Time::MS {300};
    auto connection =
        Connection {webSocketUrl("/ws"), recorder.callbacks(), options};
    check(recorder.waitForOpen());

    connection.send("ignore-close");
    connection.send("ping");
    check(recorder.waitForMessages(1));

    auto start = Clock::now();
    connection.close();
    check(recorder.waitForClose());
    check(recorder.closed->code == 1006);
    check(recorder.error.empty());
    check(millisecondsSince(start) < 3000);
};

auto tCloseBeforeOpen = test("WebWebSocket/closingBeforeOpenAbandonsTheAttempt") = []
{
    auto recorder = Recorder {};
    auto connection =
        Connection {webSocketUrl("/ws"), recorder.callbacks(), quickOptions()};
    connection.close();

    check(recorder.waitForClose());
    check(recorder.closed->code == 1006);
    check(recorder.error.empty());
};

auto tExit = test("WebWebSocket/theRuntimeExitingSilencesASocket") = []
{
    auto recorder = Recorder {};
    auto connection =
        Connection {webSocketUrl("/ws"), recorder.callbacks(), quickOptions()};
    runTheRuntimeExitCallbacks();

    eacp::Threads::runEventLoopFor(eacp::Time::MS {500});
    check(!recorder.opened);
    check(!recorder.closed);
};
} // namespace WebSocketWebTests
