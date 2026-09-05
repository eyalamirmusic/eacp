#include "Common.h"

#include <eacp/Network/WebSocket/Protocol.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace nano;
using eacp::Threads::runEventLoopUntil;
using eacp::Time::Deadline;
using eacp::Time::MS;
using eacp::WebSocket::ClientId;
using eacp::WebSocket::CloseStatus;
using eacp::WebSocket::Connection;
using eacp::WebSocket::Handshake;
using eacp::WebSocket::Message;
using eacp::WebSocket::MessageType;
using eacp::WebSocket::Options;
using eacp::WebSocket::Server;
using eacp::WebSocket::ServerCallbacks;
using eacp::WebSocket::ServerOptions;

namespace WebSocketProtocolNames = eacp::WebSocket::Protocol;

namespace
{
constexpr auto webSocketServerPumpTimeout = MS {10000};

// The RFC's own example key, so the accept header the server writes back can
// be checked against the value §1.3 spells out.
constexpr auto webSocketServerExampleKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr auto webSocketServerExampleAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

bool webSocketServerPumpUntil(const std::function<bool()>& ready)
{
    return runEventLoopUntil(ready, webSocketServerPumpTimeout);
}

void webSocketServerPumpFor(MS duration)
{
    runEventLoopUntil([] { return false; }, duration);
}

// Everything a Server said, in the order it said it, plus the three things
// the header promises about *where* it said it: on the message thread, never
// inside another callback, and never inside a Server call of our own.
struct WebSocketServerRecord
{
    struct Scope
    {
        explicit Scope(WebSocketServerRecord& recordToUse)
            : record(recordToUse)
        {
            if (!eacp::Threads::isMainThread())
                record.offMessageThread = true;

            if (record.depth > 0)
                record.reentered = true;

            if (record.inServerCall)
                record.duringAServerCall = true;

            ++record.depth;
        }

        ~Scope() { --record.depth; }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        WebSocketServerRecord& record;
    };

    ServerCallbacks callbacks()
    {
        auto result = ServerCallbacks();

        result.onConnect = [this](ClientId id, const Handshake& handshake)
        {
            auto scope = Scope(*this);
            ++connects;
            connected.push_back(id);
            handshakes[id] = handshake;
            order.emplace_back("connect");
        };

        result.onMessage = [this](ClientId id, const Message& message)
        {
            auto scope = Scope(*this);
            messages.emplace_back(id, message);
            order.emplace_back("message");
        };

        result.onDisconnect = [this](ClientId id, const CloseStatus& status)
        {
            auto scope = Scope(*this);
            ++disconnects;
            closes.emplace_back(id, status);
            order.emplace_back("disconnect");
        };

        result.onError = [this](const std::string& text)
        {
            auto scope = Scope(*this);
            ++errors;
            error = text;
            order.emplace_back("error");
        };

        return result;
    }

    std::size_t messagesFor(ClientId id) const
    {
        auto count = std::size_t {0};

        for (const auto& [from, message]: messages)
            if (from == id)
                ++count;

        return count;
    }

    CloseStatus closeOf(ClientId id) const
    {
        for (const auto& [from, status]: closes)
            if (from == id)
                return status;

        return {};
    }

    int connects = 0;
    int disconnects = 0;
    int errors = 0;
    int depth = 0;
    std::string error;
    bool offMessageThread = false;
    bool reentered = false;
    bool inServerCall = false;
    bool duringAServerCall = false;
    std::vector<ClientId> connected;
    std::map<ClientId, Handshake> handshakes;
    std::vector<std::pair<ClientId, Message>> messages;
    std::vector<std::pair<ClientId, CloseStatus>> closes;
    std::vector<std::string> order;
};

// What a WebSocket::Connection said, kept apart from the server's own record
// so a test can tell the two ends of the same socket apart.
struct WebSocketServerClientRecord
{
    eacp::WebSocket::Callbacks callbacks()
    {
        auto result = eacp::WebSocket::Callbacks();

        result.onOpen = [this](const std::string& selected)
        {
            ++opens;
            protocol = selected;
        };

        result.onMessage = [this](const Message& message)
        { messages.push_back(message); };

        result.onClose = [this](const CloseStatus& status)
        {
            ++closes;
            close = status;
        };

        result.onError = [this](const std::string&) { ++errors; };

        return result;
    }

    int opens = 0;
    int closes = 0;
    int errors = 0;
    std::string protocol;
    CloseStatus close;
    std::vector<Message> messages;
};

std::string webSocketServerUrl(const Server& server, const std::string& path = {})
{
    return "ws://127.0.0.1:" + std::to_string(server.boundPort()) + path;
}

bool webSocketServerClientSkipped()
{
    return !Connection::isSupported();
}

std::unique_ptr<Connection> webSocketServerConnect(const std::string& url,
                                                   WebSocketServerClientRecord& record,
                                                   const Options& options = {})
{
    return std::make_unique<Connection>(url, record.callbacks(), options);
}

bool webSocketServerClientOpened(WebSocketServerClientRecord& record)
{
    auto settled = [&] { return record.opens > 0 || record.closes > 0; };
    return webSocketServerPumpUntil(settled) && record.opens == 1;
}

// A peer of our own making, speaking the protocol off Protocol.h over a raw
// TCP::Connection. It is what proves the server where WebSocket::Connection is
// unsupported - Ubuntu's libcurl - and the only way to put a frame no client
// would ever send on the wire.
//
// Every blocking read pumps the loop first, because the server's own callbacks
// arrive through it and a test that only blocked here would deadlock itself.
class WebSocketServerPeer
{
public:
    explicit WebSocketServerPeer(std::uint16_t port)
        : connection(eacp::TCP::Connection::connect({"127.0.0.1", port},
                                                    {MS {2000}, MS {50}}))
    {
    }

    void writeRaw(const std::string& bytes) { connection.send(bytes); }

    void sendUpgrade(const std::string& path, const std::string& extra = {})
    {
        writeRaw("GET " + path
                 + " HTTP/1.1\r\n"
                   "Host: 127.0.0.1\r\n"
                   "Upgrade: websocket\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Key: "
                 + webSocketServerExampleKey
                 + "\r\n"
                   "Sec-WebSocket-Version: 13\r\n"
                 + extra + "\r\n");
    }

    void sendFrame(const WebSocketProtocolNames::Frame& frame)
    {
        writeRaw(WebSocketProtocolNames::encode(frame, true));
    }

    std::string readHead(MS timeout = MS {5000})
    {
        auto deadline = Deadline {timeout};

        while (!deadline.expired())
        {
            auto blank = buffer.find("\r\n\r\n");

            if (blank != std::string::npos)
            {
                auto head = buffer.substr(0, blank + 4);
                buffer.erase(0, blank + 4);
                return head;
            }

            if (!pumpAndRead())
                break;
        }

        return {};
    }

    std::optional<WebSocketProtocolNames::Frame> readFrame(MS timeout = MS {5000})
    {
        auto deadline = Deadline {timeout};

        while (!deadline.expired())
        {
            auto decoded = WebSocketProtocolNames::decode(buffer);

            if (decoded.has_value())
            {
                buffer.erase(0, decoded->consumed);
                return decoded->frame;
            }

            if (!pumpAndRead())
                break;
        }

        return std::nullopt;
    }

    bool waitForEnd(MS timeout = MS {5000})
    {
        auto deadline = Deadline {timeout};

        while (!deadline.expired())
            if (!pumpAndRead())
                return true;

        return false;
    }

    void close() { connection.close(); }

private:
    bool pumpAndRead()
    {
        webSocketServerPumpFor(MS {10});

        try
        {
            auto chunk = connection.receive(65536);

            if (chunk.empty())
                return false;

            buffer += chunk;
            return true;
        }
        catch (const eacp::TCP::TimeoutError&)
        {
            return true;
        }
        catch (const eacp::TCP::Error&)
        {
            return false;
        }
    }

    eacp::TCP::Connection connection;
    std::string buffer;
};

// A frame no encoder here will build: RSV1 set on an empty masked text frame,
// which §5.2 says a receiver must fail the connection over.
std::string webSocketServerReservedBitFrame()
{
    return std::string("\xC1\x80\x00\x00\x00\x00", 6);
}

bool webSocketServerHeadHas(const std::string& head, const std::string& line)
{
    return head.find(line) != std::string::npos;
}

std::unique_ptr<Server> webSocketServerListening(WebSocketServerRecord& record,
                                                 ServerOptions options = {})
{
    auto server = std::make_unique<Server>(record.callbacks(), std::move(options));
    server->listen(0);
    return server;
}
} // namespace

auto tServerBinds = test("WebSocketServer/bindsAnEphemeralPortAndRefusesABusyOne") =
    []
{
    auto record = WebSocketServerRecord();
    auto server = Server(record.callbacks());

    check(!server.isListening());
    check(server.boundPort() == 0);
    check(server.clientCount() == 0);

    check(server.listen(0));
    check(server.isListening());
    check(server.boundPort() != 0);

    // A server already listening keeps the port it has rather than rebinding.
    auto bound = server.boundPort();
    check(!server.listen(0));
    check(server.boundPort() == bound);

    auto taken = eacp::TCP::Listener::bind(0);
    auto other = Server(ServerCallbacks());

    check(!other.listen(taken.port()));
    check(!other.isListening());
    check(other.boundPort() == 0);
};

auto tServerHandshakeResponse =
    test("WebSocketServer/answersAnUpgradeWithTheAcceptKey") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/raw?x=1", "Origin: https://eacp.test\r\n");

    auto head = peer.readHead();

    check(webSocketServerHeadHas(head, "HTTP/1.1 101 Switching Protocols\r\n"));
    check(webSocketServerHeadHas(head, "Upgrade: websocket\r\n"));
    check(webSocketServerHeadHas(head, "Connection: Upgrade\r\n"));
    check(webSocketServerHeadHas(
        head, std::string("Sec-WebSocket-Accept: ") + webSocketServerExampleAccept));
    check(head.find("Sec-WebSocket-Protocol") == std::string::npos);

    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));
    check(record.handshakes[record.connected[0]].path == "/raw?x=1");
    check(record.handshakes[record.connected[0]].peerHost == "127.0.0.1");
    check(record.handshakes[record.connected[0]].headers["origin"]
          == "https://eacp.test");
    check(record.handshakes[record.connected[0]].protocol.empty());
    check(server->clientCount() == 1);
    check(record.errors == 0);
    check(!record.offMessageThread);
    check(!record.reentered);
};

auto tServerRejectsNonUpgrade =
    test("WebSocketServer/answers400ToARequestThatIsNoUpgrade") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.writeRaw("GET /plain HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");

    auto head = peer.readHead();
    check(webSocketServerHeadHas(head, "HTTP/1.1 400 Bad Request\r\n"));
    check(peer.waitForEnd());

    // A handshake that never happened owes neither callback, and the slot it
    // held has to go all the same.
    webSocketServerPumpFor(MS {300});
    check(record.connects == 0);
    check(record.disconnects == 0);
    check(server->clientCount() == 0);
};

auto tServerEchoesARawMessage =
    test("WebSocketServer/carriesAMaskedFrameAndAnswersUnmasked") = []
{
    auto record = WebSocketServerRecord();
    auto sending = std::unique_ptr<Server>();

    auto callbacks = record.callbacks();
    auto heard = callbacks.onMessage;

    callbacks.onMessage = [&](ClientId id, const Message& message)
    {
        heard(id, message);
        sending->send(id, message.data);
    };

    sending = std::make_unique<Server>(std::move(callbacks));
    check(sending->listen(0));

    auto peer = WebSocketServerPeer(sending->boundPort());
    peer.sendUpgrade("/echo");
    check(!peer.readHead().empty());

    peer.sendFrame({WebSocketProtocolNames::Opcode::text, true, "hello"});

    auto answer = peer.readFrame();
    check(answer.has_value());
    check(answer->opcode == WebSocketProtocolNames::Opcode::text);
    check(answer->payload == "hello");
    check(record.messages.size() == 1);
    check(record.messages[0].second.type == MessageType::text);
};

auto tServerReassemblesFragments =
    test("WebSocketServer/reassemblesAFragmentedClientMessage") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/fragments");
    check(!peer.readHead().empty());

    peer.sendFrame({WebSocketProtocolNames::Opcode::text, false, "one "});
    peer.sendFrame({WebSocketProtocolNames::Opcode::continuation, false, "two "});
    peer.sendFrame({WebSocketProtocolNames::Opcode::continuation, true, "three"});

    check(webSocketServerPumpUntil([&] { return !record.messages.empty(); }));
    webSocketServerPumpFor(MS {150});

    check(record.messages.size() == 1);
    check(record.messages[0].second.data == "one two three");
    check(record.messages[0].second.type == MessageType::text);
};

auto tServerAnswersAPing = test("WebSocketServer/answersAPingWithAPong") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/ping");
    check(!peer.readHead().empty());

    peer.sendFrame({WebSocketProtocolNames::Opcode::ping, true, "are you there"});

    auto answer = peer.readFrame();
    check(answer.has_value());
    check(answer->opcode == WebSocketProtocolNames::Opcode::pong);
    check(answer->payload == "are you there");
    check(record.messages.empty());
};

auto tServerFailsOnAReservedBit =
    test("WebSocketServer/failsAConnectionOverAReservedBit") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/malformed");
    check(!peer.readHead().empty());

    peer.writeRaw(webSocketServerReservedBitFrame());

    auto answer = peer.readFrame();
    check(answer.has_value());
    check(answer->opcode == WebSocketProtocolNames::Opcode::close);
    check(WebSocketProtocolNames::decodeClose(answer->payload).code == 1002);

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    check(record.closes[0].second.code == 1002);
    check(server->clientCount() == 0);
};

auto tServerEchoesAClose = test("WebSocketServer/echoesTheCloseAPeerSent") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/closing");
    check(!peer.readHead().empty());
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    auto payload = WebSocketProtocolNames::encodeClose(4002, "bye");
    peer.sendFrame({WebSocketProtocolNames::Opcode::close, true, payload});

    auto answer = peer.readFrame();
    check(answer.has_value());
    check(answer->opcode == WebSocketProtocolNames::Opcode::close);

    auto echoed = WebSocketProtocolNames::decodeClose(answer->payload);
    check(echoed.code == 4002);
    check(echoed.reason == "bye");

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    check(record.closes[0].second.code == 4002);
    check(record.closes[0].second.reason == "bye");
    check(server->clientCount() == 0);
    check(record.errors == 0);
    check(!record.offMessageThread);
    check(!record.reentered);
};

auto tServerChoosesTheProtocol =
    test("WebSocketServer/agreesToTheFirstProtocolOfferedThatItKnows") = []
{
    auto record = WebSocketServerRecord();

    auto options = ServerOptions();
    options.protocols.add("chat");
    options.protocols.add("log");

    auto server = webSocketServerListening(record, options);

    auto matching = WebSocketServerPeer(server->boundPort());
    matching.sendUpgrade("/a", "Sec-WebSocket-Protocol: log, chat\r\n");
    check(webSocketServerHeadHas(matching.readHead(),
                                 "Sec-WebSocket-Protocol: log\r\n"));

    auto unknown = WebSocketServerPeer(server->boundPort());
    unknown.sendUpgrade("/b", "Sec-WebSocket-Protocol: x\r\n");
    auto head = unknown.readHead();
    check(webSocketServerHeadHas(head, "101 Switching Protocols"));
    check(head.find("Sec-WebSocket-Protocol") == std::string::npos);

    check(webSocketServerPumpUntil([&] { return record.connects == 2; }));
    check(record.handshakes[record.connected[0]].protocol == "log");
    check(record.handshakes[record.connected[1]].protocol.empty());
};

auto tServerAgreesToNothing =
    test("WebSocketServer/agreesToNoProtocolWhereItNamesNone") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/none", "Sec-WebSocket-Protocol: chat\r\n");

    auto head = peer.readHead();
    check(webSocketServerHeadHas(head, "101 Switching Protocols"));
    check(head.find("Sec-WebSocket-Protocol") == std::string::npos);

    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));
    check(record.handshakes[record.connected[0]].protocol.empty());
};

auto tServerIdsAreUnique = test("WebSocketServer/givesEachClientItsOwnRisingId") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto first = WebSocketServerPeer(server->boundPort());
    first.sendUpgrade("/one");
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    auto second = WebSocketServerPeer(server->boundPort());
    second.sendUpgrade("/two");
    check(webSocketServerPumpUntil([&] { return record.connects == 2; }));

    check(record.connected[0] != record.connected[1]);
    check(record.connected[0] < record.connected[1]);
    check(server->clientCount() == 2);
};

auto tServerStopsWithRawPeers =
    test("WebSocketServer/stopCloses1001AndGoesQuiet") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);
    auto port = server->boundPort();

    auto first = WebSocketServerPeer(port);
    first.sendUpgrade("/one");
    check(!first.readHead().empty());

    auto second = WebSocketServerPeer(port);
    second.sendUpgrade("/two");
    check(!second.readHead().empty());

    check(webSocketServerPumpUntil([&] { return record.connects == 2; }));

    auto before = record.order.size();
    server->stop();

    check(!server->isListening());
    check(server->clientCount() == 0);

    for (auto* peer: {&first, &second})
    {
        auto answer = peer->readFrame();
        check(answer.has_value());
        check(answer->opcode == WebSocketProtocolNames::Opcode::close);
        check(WebSocketProtocolNames::decodeClose(answer->payload).code == 1001);
    }

    // Nothing the teardown provoked reaches a Server that has stopped.
    webSocketServerPumpFor(MS {200});
    check(record.order.size() == before);
    check(record.disconnects == 0);

    auto refused = false;

    try
    {
        auto again = eacp::TCP::Connection::connect({"127.0.0.1", port},
                                                    {MS {500}, MS {500}});
    }
    catch (const eacp::TCP::Error&)
    {
        refused = true;
    }

    check(refused);
};

auto tServerListensAgain = test("WebSocketServer/listensAgainAfterAStop") = []
{
    auto record = WebSocketServerRecord();
    auto server = Server(record.callbacks());

    check(server.listen(0));
    auto first = server.boundPort();

    server.stop();
    check(!server.isListening());
    check(server.boundPort() == 0);

    check(server.listen(0));
    check(server.isListening());
    check(server.boundPort() != 0);
    check(server.boundPort() != first || first != 0);

    auto peer = WebSocketServerPeer(server.boundPort());
    peer.sendUpgrade("/again");
    check(!peer.readHead().empty());
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));
};

auto tServerDestroyedPromptly =
    test("WebSocketServer/destroyingItWithPeersOnReturnsAtOnce") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto first = WebSocketServerPeer(server->boundPort());
    first.sendUpgrade("/one");
    check(!first.readHead().empty());

    auto second = WebSocketServerPeer(server->boundPort());
    second.sendUpgrade("/two");
    check(!second.readHead().empty());

    check(webSocketServerPumpUntil([&] { return record.connects == 2; }));

    auto before = record.order.size();
    auto promptly = Deadline {MS {1000}};
    server.reset();
    check(!promptly.expired());

    webSocketServerPumpFor(MS {200});
    check(record.order.size() == before);

    check(first.waitForEnd());
    check(second.waitForEnd());
};

auto tServerBroadcastsToRawPeers =
    test("WebSocketServer/broadcastReachesEveryConnectedPeer") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto first = WebSocketServerPeer(server->boundPort());
    first.sendUpgrade("/one");
    check(!first.readHead().empty());

    auto second = WebSocketServerPeer(server->boundPort());
    second.sendUpgrade("/two");
    check(!second.readHead().empty());

    check(webSocketServerPumpUntil([&] { return record.connects == 2; }));

    record.inServerCall = true;
    server->broadcast("to everyone");
    record.inServerCall = false;

    for (auto* peer: {&first, &second})
    {
        auto answer = peer->readFrame();
        check(answer.has_value());
        check(answer->opcode == WebSocketProtocolNames::Opcode::text);
        check(answer->payload == "to everyone");
    }

    check(!record.duringAServerCall);
};

auto tServerCloseWaitsForTheEcho =
    test("WebSocketServer/aServerCloseReportsWhatThePeerEchoed") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/kick");
    check(!peer.readHead().empty());
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    auto id = record.connected[0];
    record.inServerCall = true;
    server->close(id, 4001, "kick");
    record.inServerCall = false;

    auto sent = peer.readFrame();
    check(sent.has_value());
    check(sent->opcode == WebSocketProtocolNames::Opcode::close);

    auto asked = WebSocketProtocolNames::decodeClose(sent->payload);
    check(asked.code == 4001);
    check(asked.reason == "kick");

    // Nothing is reported until the peer answers, and what is reported then is
    // the peer's own status.
    webSocketServerPumpFor(MS {200});
    check(record.disconnects == 0);

    peer.sendFrame({WebSocketProtocolNames::Opcode::close, true, sent->payload});

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    check(record.closeOf(id).code == 4001);
    check(record.closeOf(id).reason == "kick");
    check(server->clientCount() == 0);
    check(!record.duringAServerCall);
};

auto tServerCloseTimesOut =
    test("WebSocketServer/aCloseNobodyAnswersEndsAs1006") = []
{
    auto record = WebSocketServerRecord();

    auto options = ServerOptions();
    options.closeTimeout = MS {400};

    auto server = webSocketServerListening(record, options);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/silent");
    check(!peer.readHead().empty());
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    auto id = record.connected[0];
    server->close(id, 1000);

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    check(record.closeOf(id).code == 1006);
    check(server->clientCount() == 0);
};

auto tServerDropsSendsAfterAClose =
    test("WebSocketServer/dropsSendsForAnIdItDoesNotHold") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/gone");
    check(!peer.readHead().empty());
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    auto id = record.connected[0];

    server->send(id + 1000, "nobody");
    server->sendBinary(id + 1000, "nobody");
    server->close(id + 1000);

    server->close(id, 1000);
    server->send(id, "after the close");

    auto sent = peer.readFrame();
    check(sent.has_value());
    check(sent->opcode == WebSocketProtocolNames::Opcode::close);

    peer.sendFrame({WebSocketProtocolNames::Opcode::close, true, sent->payload});
    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));

    server->send(id, "after the disconnect");
    webSocketServerPumpFor(MS {150});
    check(record.disconnects == 1);
};

auto tServerLimitsTheMessage =
    test("WebSocketServer/closes1009OverMaxMessageSize") = []
{
    auto record = WebSocketServerRecord();

    auto options = ServerOptions();
    options.maxMessageSize = 1024;

    auto server = webSocketServerListening(record, options);

    auto peer = WebSocketServerPeer(server->boundPort());
    peer.sendUpgrade("/big");
    check(!peer.readHead().empty());

    peer.sendFrame(
        {WebSocketProtocolNames::Opcode::text, true, std::string(4096, 'x')});

    auto answer = peer.readFrame();
    check(answer.has_value());
    check(answer->opcode == WebSocketProtocolNames::Opcode::close);
    check(WebSocketProtocolNames::decodeClose(answer->payload).code == 1009);

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    check(record.closes[0].second.code == 1009);
    check(record.messages.empty());
};

auto tServerHearsAPeerGoing =
    test("WebSocketServer/aPeerThatVanishesDisconnectsAs1006") = []
{
    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto peer = std::make_optional<WebSocketServerPeer>(server->boundPort());
    peer->sendUpgrade("/vanishing");
    check(!peer->readHead().empty());
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    peer.reset();

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    check(record.closes[0].second.code == 1006);
    check(record.disconnects == 1);
    check(server->clientCount() == 0);
};

auto tServerMeetsTheClient =
    test("WebSocketServer/meetsAWebSocketConnectionAndReportsIt") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);
    auto client = WebSocketServerClientRecord();

    auto options = Options();
    options.headers["Origin"] = "https://eacp.test";

    auto connection =
        webSocketServerConnect(webSocketServerUrl(*server, "/chat?x=1"), client,
                               options);

    check(webSocketServerClientOpened(client));
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    auto id = record.connected[0];
    check(record.handshakes[id].path == "/chat?x=1");
    check(record.handshakes[id].peerHost == "127.0.0.1");
    check(record.handshakes[id].headers["origin"] == "https://eacp.test");
    check(record.handshakes[id].headers.count("sec-websocket-key") == 1);
    check(server->clientCount() == 1);
    check(!record.offMessageThread);
    check(!record.reentered);

    auto secondClient = WebSocketServerClientRecord();
    auto second =
        webSocketServerConnect(webSocketServerUrl(*server), secondClient);

    check(webSocketServerClientOpened(secondClient));
    check(webSocketServerPumpUntil([&] { return record.connects == 2; }));
    check(record.connected[0] < record.connected[1]);
    check(server->clientCount() == 2);
};

auto tServerPicksTheClientsProtocol =
    test("WebSocketServer/picksASubprotocolAWebSocketConnectionOffered") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto record = WebSocketServerRecord();

    auto serverOptions = ServerOptions();
    serverOptions.protocols.add("chat");
    serverOptions.protocols.add("log");

    auto server = webSocketServerListening(record, serverOptions);
    auto client = WebSocketServerClientRecord();

    auto options = Options();
    options.protocols.add("log");
    options.protocols.add("chat");

    auto connection =
        webSocketServerConnect(webSocketServerUrl(*server), client, options);

    check(webSocketServerClientOpened(client));
    check(client.protocol == "log");
    check(connection->protocol() == "log");

    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));
    check(record.handshakes[record.connected[0]].protocol == "log");
};

auto tServerEchoesForTheClient =
    test("WebSocketServer/echoesTextAndBinaryToAWebSocketConnection") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto record = WebSocketServerRecord();
    auto server = std::unique_ptr<Server>();

    auto callbacks = record.callbacks();
    auto heard = callbacks.onMessage;

    callbacks.onMessage = [&](ClientId id, const Message& message)
    {
        heard(id, message);

        if (message.type == MessageType::binary)
            server->sendBinary(id, message.data);
        else
            server->send(id, message.data);
    };

    server = std::make_unique<Server>(std::move(callbacks));
    check(server->listen(0));

    auto client = WebSocketServerClientRecord();
    auto connection = webSocketServerConnect(webSocketServerUrl(*server), client);
    check(webSocketServerClientOpened(client));

    connection->send("hello");
    check(webSocketServerPumpUntil([&] { return client.messages.size() == 1; }));
    check(client.messages[0].data == "hello");
    check(client.messages[0].type == MessageType::text);

    auto bytes = std::string();

    for (auto value = 0; value < 256; ++value)
        bytes.push_back((char) value);

    connection->sendBinary(bytes);
    check(webSocketServerPumpUntil([&] { return client.messages.size() == 2; }));
    check(client.messages[1].type == MessageType::binary);
    check(client.messages[1].data == bytes);

    constexpr auto burst = 50;

    for (auto i = 0; i < burst; ++i)
        connection->send("m" + std::to_string(i));

    auto allBack = [&] { return client.messages.size() == burst + 2; };
    check(webSocketServerPumpUntil(allBack));

    auto inOrder = true;

    for (auto i = 0; i < burst; ++i)
        inOrder = inOrder
                  && client.messages[(std::size_t) i + 2].data
                         == "m" + std::to_string(i);

    check(inOrder);
    check(client.errors == 0);
    check(!record.offMessageThread);
    check(!record.reentered);
};

auto tServerCarriesThreeMegabytes =
    test("WebSocketServer/carriesThreeMegabytesBothWays") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto payload = std::string(3 * 1024 * 1024, 'z');

    auto record = WebSocketServerRecord();
    auto server = std::unique_ptr<Server>();

    auto callbacks = record.callbacks();
    auto heard = callbacks.onMessage;

    callbacks.onMessage = [&](ClientId id, const Message& message)
    {
        heard(id, message);
        server->sendBinary(id, message.data);
    };

    server = std::make_unique<Server>(std::move(callbacks));
    check(server->listen(0));

    auto client = WebSocketServerClientRecord();
    auto connection = webSocketServerConnect(webSocketServerUrl(*server), client);
    check(webSocketServerClientOpened(client));

    connection->sendBinary(payload);

    check(webSocketServerPumpUntil([&] { return !client.messages.empty(); }));
    check(record.messages.size() == 1);
    check(record.messages[0].second.data.size() == payload.size());
    check(record.messages[0].second.data == payload);
    check(client.messages[0].data.size() == payload.size());
    check(client.messages[0].data == payload);
    check(client.errors == 0);
};

auto tServerBroadcastsToClients =
    test("WebSocketServer/broadcastReachesTwoWebSocketConnections") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto first = WebSocketServerClientRecord();
    auto firstConnection =
        webSocketServerConnect(webSocketServerUrl(*server), first);
    check(webSocketServerClientOpened(first));

    auto second = WebSocketServerClientRecord();
    auto secondConnection =
        webSocketServerConnect(webSocketServerUrl(*server), second);
    check(webSocketServerClientOpened(second));

    check(webSocketServerPumpUntil([&] { return record.connects == 2; }));

    server->broadcast("to everyone");

    auto bothHeard = [&]
    { return !first.messages.empty() && !second.messages.empty(); };

    check(webSocketServerPumpUntil(bothHeard));
    check(first.messages[0].data == "to everyone");
    check(second.messages[0].data == "to everyone");
};

auto tServerClosesAClient =
    test("WebSocketServer/closingAWebSocketConnectionCarriesTheCode") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto client = WebSocketServerClientRecord();
    auto connection = webSocketServerConnect(webSocketServerUrl(*server), client);
    check(webSocketServerClientOpened(client));
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    auto id = record.connected[0];
    server->close(id, 4001, "kick");

    check(webSocketServerPumpUntil([&] { return client.closes == 1; }));
    check(client.close.code == 4001);
    check(client.close.reason == "kick");

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    check(record.closeOf(id).code == 4001);
    check(server->clientCount() == 0);
    check(client.errors == 0);
};

auto tClientClosesTheServer =
    test("WebSocketServer/aWebSocketConnectionClosingIsReportedWithItsCode") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto client = WebSocketServerClientRecord();
    auto connection = webSocketServerConnect(webSocketServerUrl(*server), client);
    check(webSocketServerClientOpened(client));
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    auto id = record.connected[0];
    connection->close(4002, "bye");

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    check(record.closeOf(id).code == 4002);
    check(record.closeOf(id).reason == "bye");

    check(webSocketServerPumpUntil([&] { return client.closes == 1; }));
    check(client.close.code == 4002);
    check(client.close.reason == "bye");
    check(server->clientCount() == 0);
};

auto tServerHearsAClientGoing =
    test("WebSocketServer/aWebSocketConnectionDestroyedEndsExactlyOnce") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto client = WebSocketServerClientRecord();
    auto connection = webSocketServerConnect(webSocketServerUrl(*server), client);
    check(webSocketServerClientOpened(client));
    check(webSocketServerPumpUntil([&] { return record.connects == 1; }));

    connection.reset();

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    webSocketServerPumpFor(MS {300});

    // macOS's invalidateAndCancel puts a close frame with no payload on the
    // wire, which §7.4.1 reads back as 1005 - so what the server reports is a
    // close the peer sent, not the 1006 a transport merely ending would give.
    // A backend that dropped the socket instead would land on 1006.
    check(record.disconnects == 1);
    check(record.closes[0].second.code == 1005
          || record.closes[0].second.code == 1006);
    check(record.closes[0].second.reason.empty());
    check(server->clientCount() == 0);
};

auto tServerLimitsAClientMessage =
    test("WebSocketServer/aWebSocketConnectionOverTheLimitIsClosed") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto record = WebSocketServerRecord();

    auto options = ServerOptions();
    options.maxMessageSize = 1024;

    auto server = webSocketServerListening(record, options);

    auto client = WebSocketServerClientRecord();
    auto connection = webSocketServerConnect(webSocketServerUrl(*server), client);
    check(webSocketServerClientOpened(client));

    connection->send(std::string(4096, 'x'));

    check(webSocketServerPumpUntil([&] { return record.disconnects == 1; }));
    check(record.closes[0].second.code == 1009);
    check(record.messages.empty());

    check(webSocketServerPumpUntil([&] { return client.closes == 1; }));
    check(client.close.code == 1009 || client.close.code == 1006);
};

auto tServerStopsWithClients =
    test("WebSocketServer/stoppingClosesEveryWebSocketConnectionWith1001") = []
{
    if (webSocketServerClientSkipped())
        return;

    auto record = WebSocketServerRecord();
    auto server = webSocketServerListening(record);

    auto first = WebSocketServerClientRecord();
    auto firstConnection =
        webSocketServerConnect(webSocketServerUrl(*server), first);
    check(webSocketServerClientOpened(first));

    auto second = WebSocketServerClientRecord();
    auto secondConnection =
        webSocketServerConnect(webSocketServerUrl(*server), second);
    check(webSocketServerClientOpened(second));

    check(webSocketServerPumpUntil([&] { return record.connects == 2; }));

    auto before = record.order.size();
    server->stop();

    check(!server->isListening());
    check(server->clientCount() == 0);

    auto bothClosed = [&] { return first.closes == 1 && second.closes == 1; };
    check(webSocketServerPumpUntil(bothClosed));

    check(first.close.code == 1001);
    check(second.close.code == 1001);
    check(record.order.size() == before);
    check(!record.offMessageThread);
    check(!record.reentered);
};
