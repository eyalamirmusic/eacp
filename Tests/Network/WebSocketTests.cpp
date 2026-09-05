#include "Common.h"
#include "WebSocketTestServer.h"

#include <memory>
#include <vector>

using namespace nano;
using eacp::Threads::runEventLoopUntil;
using eacp::Time::MS;
using eacp::WebSocket::Callbacks;
using eacp::WebSocket::CloseStatus;
using eacp::WebSocket::Connection;
using eacp::WebSocket::Message;
using eacp::WebSocket::MessageType;
using eacp::WebSocket::Options;
using eacp::WebSocket::State;

namespace
{
constexpr auto webSocketPumpTimeout = MS {10000};

// Everything a connection said, in the order it said it, and whether any of
// it arrived somewhere other than the message thread - which is half of what
// these tests are checking.
struct WebSocketRecord
{
    Callbacks callbacks()
    {
        auto result = Callbacks();

        result.onOpen = [this](const std::string& selected)
        {
            noteThread();
            ++opens;
            protocol = selected;
            order.emplace_back("open");
        };

        result.onMessage = [this](const Message& message)
        {
            noteThread();
            messages.push_back(message);
            order.emplace_back("message");
        };

        result.onClose = [this](const CloseStatus& status)
        {
            noteThread();
            ++closes;
            close = status;
            order.emplace_back("close");
        };

        result.onError = [this](const std::string& text)
        {
            noteThread();
            ++errors;
            error = text;
            order.emplace_back("error");
        };

        return result;
    }

    void noteThread()
    {
        if (!eacp::Threads::isMainThread())
            offMessageThread = true;
    }

    int opens = 0;
    int closes = 0;
    int errors = 0;
    std::string protocol;
    std::string error;
    CloseStatus close;
    std::vector<Message> messages;
    std::vector<std::string> order;
    bool offMessageThread = false;
};

bool webSocketPumpUntil(const std::function<bool()>& ready)
{
    return runEventLoopUntil(ready, webSocketPumpTimeout);
}

void webSocketPumpFor(MS duration)
{
    runEventLoopUntil([] { return false; }, duration);
}

// A libcurl built without the protocol answers false, and every case below
// is about a socket that would never open there.
bool webSocketSkipped()
{
    return !Connection::isSupported();
}

std::unique_ptr<Connection> webSocketConnect(const std::string& url,
                                             WebSocketRecord& record,
                                             const Options& options = {})
{
    return std::make_unique<Connection>(url, record.callbacks(), options);
}

bool webSocketOpened(WebSocketRecord& record)
{
    return webSocketPumpUntil([&] { return record.opens > 0 || record.closes > 0; })
           && record.opens == 1;
}

std::string webSocketBytesOfEveryValue()
{
    auto payload = std::string();

    for (auto value = 0; value < 256; ++value)
        payload.push_back((char) value);

    return payload;
}
} // namespace

auto tSupported = test("WebSocket/isSupportedOnThisPlatform") = []
{
#if defined(__APPLE__)
    check(Connection::isSupported());
#else
    check(true);
#endif
};

auto tOpensWithSubprotocol =
    test("WebSocket/opensAndReportsTheChosenSubprotocol") = []
{
    if (webSocketSkipped())
        return;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto options = Options();
    options.protocols.add("chat");
    options.protocols.add("superchat");

    auto connection = webSocketConnect(server.url(), record, options);
    check(connection->state() == State::connecting);
    check(record.opens == 0);

    check(webSocketOpened(record));
    check(connection->state() == State::open);
    check(record.protocol == "chat");
    check(connection->protocol() == "chat");
    check(!record.offMessageThread);
};

auto tOpensWithoutSubprotocol = test("WebSocket/opensWithNoSubprotocolOffered") = []
{
    if (webSocketSkipped())
        return;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);

    check(webSocketOpened(record));
    check(record.protocol.empty());
    check(connection->protocol().empty());
    check(server.requestHeader("sec-websocket-protocol").empty());
};

auto tCustomHeader = test("WebSocket/carriesACustomRequestHeader") = []
{
    if (webSocketSkipped())
        return;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto options = Options();
    options.headers["Origin"] = "https://eacp.test";

    auto connection = webSocketConnect(server.url(), record, options);

    check(webSocketOpened(record));
    check(server.requestHeader("Origin") == "https://eacp.test");
    check(server.requestHeader("sec-websocket-version") == "13");
};

auto tTextEcho = test("WebSocket/echoesTextAndKeepsTheOrderOfABurst") = []
{
    if (webSocketSkipped())
        return;

    constexpr auto burst = 50;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);
    check(webSocketOpened(record));

    connection->send("hello");
    check(webSocketPumpUntil([&] { return record.messages.size() == 1; }));
    check(record.messages[0].data == "hello");
    check(record.messages[0].type == MessageType::text);

    for (auto i = 0; i < burst; ++i)
        connection->send("m" + std::to_string(i));

    auto allBack = [&] { return record.messages.size() == burst + 1; };
    check(webSocketPumpUntil(allBack));

    auto inOrder = true;

    for (auto i = 0; i < burst; ++i)
        inOrder =
            inOrder
            && record.messages[(std::size_t) i + 1].data == "m" + std::to_string(i);

    check(inOrder);
    check(!record.offMessageThread);
};

auto tBinaryEcho = test("WebSocket/echoesEveryByteValueAsBinary") = []
{
    if (webSocketSkipped())
        return;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);
    check(webSocketOpened(record));

    auto payload = webSocketBytesOfEveryValue();
    connection->sendBinary(payload);

    check(webSocketPumpUntil([&] { return record.messages.size() == 1; }));
    check(record.messages[0].type == MessageType::binary);
    check(record.messages[0].data.size() == payload.size());
    check(record.messages[0].data == payload);
};

auto tLargeMessage = test("WebSocket/carriesAMessageBiggerThanTheDefaultLimit") = []
{
    if (webSocketSkipped())
        return;

    // Three megabytes is past NSURLSession's own one-megabyte default, so
    // this only passes where Options::maxMessageSize reached the task.
    auto payload = std::string(3 * 1024 * 1024, 'z');

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);
    check(webSocketOpened(record));

    connection->sendBinary(payload);

    check(webSocketPumpUntil([&] { return record.messages.size() == 1; }));
    check(server.messages().size() == 1);
    check(server.messages()[0].size() == payload.size());
    check(record.messages[0].data.size() == payload.size());
    check(record.messages[0].data == payload);
    check(record.errors == 0);
};

auto tMessageOverTheLimit = test("WebSocket/failsOnAMessageOverMaxMessageSize") = []
{
    if (webSocketSkipped())
        return;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto options = Options();
    options.maxMessageSize = 1024;

    auto connection = webSocketConnect(server.url(), record, options);
    check(webSocketOpened(record));

    connection->send(std::string(4096, 'x'));

    check(webSocketPumpUntil([&] { return record.closes > 0; }));
    webSocketPumpFor(MS {200});

    check(record.errors == 1);
    check(record.closes == 1);
    check(record.close.code == 1006 || record.close.code == 1009);
    check(connection->state() == State::closed);
};

auto tGreeting = test("WebSocket/deliversAMessageSentRightAfterTheHandshake") = []
{
    if (webSocketSkipped())
        return;

    auto options = WebSocketTestServerOptions();
    options.greeting = "pushed-before-anyone-asked";

    auto server = WebSocketTestServer(options);
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);

    check(webSocketPumpUntil([&] { return record.messages.size() == 1; }));
    check(record.messages[0].data == options.greeting);
    check(record.opens == 1);
    check(record.order[0] == "open");
};

auto tFragmentedMessage = test("WebSocket/reassemblesAFragmentedServerMessage") = []
{
    if (webSocketSkipped())
        return;

    auto options = WebSocketTestServerOptions();
    options.greeting =
        std::string(300, 'a') + std::string(300, 'b') + std::string(300, 'c');
    options.fragmentGreeting = true;

    auto server = WebSocketTestServer(options);
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);

    check(webSocketPumpUntil([&] { return record.messages.size() == 1; }));
    webSocketPumpFor(MS {150});

    check(record.messages.size() == 1);
    check(record.messages[0].data == options.greeting);
};

auto tPingIsAnswered = test("WebSocket/answersAServerPingWithAPong") = []
{
    if (webSocketSkipped())
        return;

    auto options = WebSocketTestServerOptions();
    options.pingAfterHandshake = true;

    auto server = WebSocketTestServer(options);
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);
    check(webSocketOpened(record));

    check(webSocketPumpUntil([&] { return server.sawPong(); }));
    check(record.errors == 0);
    check(record.messages.empty());
};

auto tClientClose = test("WebSocket/aClientCloseIsAnsweredAndReported") = []
{
    if (webSocketSkipped())
        return;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);
    check(webSocketOpened(record));

    connection->close(4000, "bye");
    check(connection->state() == State::closing);

    check(webSocketPumpUntil([&] { return record.closes > 0; }));
    webSocketPumpFor(MS {200});

    check(server.sawClose());
    check(server.closeStatus().code == 4000);
    check(server.closeStatus().reason == "bye");
    check(record.closes == 1);
    check(record.errors == 0);
    check(record.close.code == 4000);
    check(record.close.reason == "bye");
    check(connection->state() == State::closed);
};

auto tCloseIsIdempotent = test("WebSocket/closingTwiceReportsOneClose") = []
{
    if (webSocketSkipped())
        return;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);
    check(webSocketOpened(record));

    connection->close(4001, "first");
    connection->close(4002, "second");

    check(webSocketPumpUntil([&] { return record.closes > 0; }));
    webSocketPumpFor(MS {200});

    check(record.closes == 1);
    check(record.errors == 0);
    check(record.close.code == 4001);
};

// A peer that takes the close frame and then answers nothing at all. What
// every backend owes is that the connection ends anyway, once, quietly, and
// promptly: closeTimeout bounds the wait where a backend waits for the
// answer, and NSURLSession waits for none.
//
// The code is where they part. A backend that reads the peer's close frame
// knows one never came and reports §7.1.5's 1006; NSURLSession's
// didCloseWithCode: fires for our own cancelWithCloseCode: as well, carrying
// back the code we passed, so macOS cannot tell an answered close from an
// abandoned one and reports what was asked for.
auto tCloseTimesOut = test("WebSocket/aCloseNobodyAnswersStillEnds") = []
{
    if (webSocketSkipped())
        return;

    auto serverOptions = WebSocketTestServerOptions();
    serverOptions.stallOnClose = true;

    auto server = WebSocketTestServer(serverOptions);
    auto record = WebSocketRecord();

    auto options = Options();
    options.closeTimeout = MS {300};

    auto connection = webSocketConnect(server.url(), record, options);
    check(webSocketOpened(record));

    connection->close(1000);
    check(connection->state() == State::closing);

    auto reported = [&] { return record.closes > 0; };
    check(runEventLoopUntil(reported, MS {2000}));

    check(record.closes == 1);
    check(record.errors == 0);
    check(record.close.code == 1006 || record.close.code == 1000);
    check(connection->state() == State::closed);
    check(server.sawClose());
    check(server.closeStatus().code == 1000);

    // Whichever answered, the transport is gone, so the peer that would not
    // finish the handshake sees the stream end all the same.
    check(webSocketPumpUntil([&] { return server.sawPeerEnd(); }));

    server.release();
};

auto tServerClose = test("WebSocket/aServerCloseArrivesWithItsCodeAndReason") = []
{
    if (webSocketSkipped())
        return;

    auto options = WebSocketTestServerOptions();
    options.closeAfterHandshake = true;
    options.closeCode = 1001;
    options.closeReason = "going away";

    auto server = WebSocketTestServer(options);
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);

    check(webSocketPumpUntil([&] { return record.closes > 0; }));
    webSocketPumpFor(MS {200});

    check(record.closes == 1);
    check(record.errors == 0);
    check(record.close.code == 1001);
    check(record.close.reason == "going away");
    check(connection->state() == State::closed);
};

auto tConnectionRefused = test("WebSocket/aRefusedConnectionErrorsThenCloses") = []
{
    if (webSocketSkipped())
        return;

    auto record = WebSocketRecord();
    auto connection = webSocketConnect("ws://127.0.0.1:9/nobody-listening", record);

    check(webSocketPumpUntil([&] { return record.closes > 0; }));
    webSocketPumpFor(MS {200});

    check(record.errors == 1);
    check(record.closes == 1);
    check(record.opens == 0);
    check(!record.error.empty());
    check(record.close.code == 1006);
    check(record.order.size() == 2);
    check(record.order[0] == "error");
    check(record.order[1] == "close");
};

auto tRejectedHandshake = test("WebSocket/aRejectedHandshakeErrorsThenCloses") = []
{
    if (webSocketSkipped())
        return;

    auto options = WebSocketTestServerOptions();
    options.rejectHandshake = true;

    auto server = WebSocketTestServer(options);
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);

    check(webSocketPumpUntil([&] { return record.closes > 0; }));
    webSocketPumpFor(MS {200});

    check(!server.sawHandshake());
    check(record.opens == 0);
    check(record.errors == 1);
    check(record.closes == 1);
    check(record.close.code == 1006);
};

auto tDestroyWhileOpen = test("WebSocket/destroyingAnOpenConnectionGoesQuiet") = []
{
    if (webSocketSkipped())
        return;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);
    check(webSocketOpened(record));

    auto before = record.order.size();
    connection.reset();

    webSocketPumpFor(MS {200});

    check(record.order.size() == before);
    check(record.closes == 0);
    check(record.errors == 0);
    check(webSocketPumpUntil([&] { return server.sawDisconnect(); }));
};

auto tDestroyWhileConnecting =
    test("WebSocket/destroyingAConnectingConnectionReturnsAtOnce") = []
{
    if (webSocketSkipped())
        return;

    auto options = WebSocketTestServerOptions();
    options.stall = true;

    auto server = WebSocketTestServer(options);
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);
    webSocketPumpFor(MS {150});

    check(record.order.empty());

    auto promptly = eacp::Time::Deadline {MS {1000}};
    connection.reset();
    check(!promptly.expired());

    webSocketPumpFor(MS {200});
    check(record.order.empty());
};

auto tCloseWhileConnecting =
    test("WebSocket/closingWhileConnectingReportsOnlyAClose") = []
{
    if (webSocketSkipped())
        return;

    auto options = WebSocketTestServerOptions();
    options.stall = true;

    auto server = WebSocketTestServer(options);
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);
    webSocketPumpFor(MS {100});

    check(connection->state() == State::connecting);
    connection->close();
    check(connection->state() == State::closing);

    check(webSocketPumpUntil([&] { return record.closes > 0; }));
    webSocketPumpFor(MS {200});

    check(record.opens == 0);
    check(record.errors == 0);
    check(record.closes == 1);
    check(record.close.code == 1006);
    check(connection->state() == State::closed);
};

auto tSendBeforeOpen = test("WebSocket/aSendBeforeOpenIsDropped") = []
{
    if (webSocketSkipped())
        return;

    auto server = WebSocketTestServer();
    auto record = WebSocketRecord();

    auto connection = webSocketConnect(server.url(), record);

    check(connection->state() == State::connecting);
    connection->send("too early");
    connection->sendBinary("too early as well");

    check(webSocketOpened(record));
    webSocketPumpFor(MS {250});

    check(server.messageCount() == 0);
    check(record.messages.empty());
    check(record.errors == 0);
};
