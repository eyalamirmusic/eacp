// Both ends of the WebSocket API in one process: a server and a client
// holding a short conversation, printed as it happens.
//
//   WebSocketDemo                      both ends, over an ephemeral port
//   WebSocketDemo --serve [PORT]       echo server on loopback (8765)
//   WebSocketDemo --connect URL [MSG]  one message to a server, one reply
//
// Talk to --serve from a browser console:
//
//   const ws = new WebSocket("ws://localhost:8765");
//   ws.onmessage = e => console.log(e.data);
//   ws.send("hi");
//
// or from a second copy of this app:
//
//   WebSocketDemo --connect ws://localhost:8765 hi
//
// wss:// needs nothing extra on macOS and Windows; on Linux it works only
// where libcurl was built with WebSocket support.

#include <eacp/Network/Network.h>

#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace eacp;

namespace
{
constexpr auto demoTimeout = Time::MS {10000};

void say(const std::string& line)
{
    std::cout << line << std::endl;
}

std::string described(const WebSocket::Message& message)
{
    if (message.type == WebSocket::MessageType::binary)
        return std::to_string(message.data.size()) + " binary bytes";

    return "\"" + message.data + "\"";
}

std::string described(const WebSocket::CloseStatus& status)
{
    return std::to_string(status.code) + " \"" + status.reason + "\"";
}

std::string named(WebSocket::ClientId client)
{
    return "client " + std::to_string(client);
}

bool clientIsSupported()
{
    if (WebSocket::Connection::isSupported())
        return true;

    say("WebSocket client unsupported on this platform's HTTP stack");
    return false;
}

// The client and the server are made with their callbacks, so a callback
// reaches the end it belongs to through here rather than through capture.
struct Dialogue
{
    WebSocket::Server* server = nullptr;
    WebSocket::Connection* client = nullptr;
    int repliesWanted = 2;
    bool clientClosed = false;
    bool serverSawTheLeaving = false;
    bool failed = false;
};

bool dialogueIsOver(const Dialogue& dialogue)
{
    return dialogue.failed
           || (dialogue.clientClosed && dialogue.serverSawTheLeaving);
}

void endDialogueWhenBothEndsAreDone(Dialogue& dialogue)
{
    if (dialogueIsOver(dialogue))
        Threads::stopEventLoop();
}

WebSocket::ServerCallbacks dialogueServerCallbacks(Dialogue& dialogue)
{
    auto callbacks = WebSocket::ServerCallbacks();

    callbacks.onConnect =
        [](WebSocket::ClientId id, const WebSocket::Handshake& handshake)
    {
        say("server: " + named(id) + " opened " + handshake.path + " as \""
            + handshake.protocol + "\"");
    };

    callbacks.onMessage =
        [&dialogue](WebSocket::ClientId id, const WebSocket::Message& message)
    {
        say("server: heard " + described(message) + " from " + named(id));

        if (message.type == WebSocket::MessageType::binary)
            dialogue.server->send(
                id, std::to_string(message.data.size()) + " bytes received");
        else
            dialogue.server->send(id, "server got: " + message.data);
    };

    callbacks.onDisconnect =
        [&dialogue](WebSocket::ClientId id, const WebSocket::CloseStatus& status)
    {
        say("server: " + named(id) + " left with " + described(status));
        dialogue.serverSawTheLeaving = true;
        endDialogueWhenBothEndsAreDone(dialogue);
    };

    callbacks.onError = [&dialogue](const std::string& error)
    {
        say("server: error " + error);
        dialogue.failed = true;
        endDialogueWhenBothEndsAreDone(dialogue);
    };

    return callbacks;
}

WebSocket::Callbacks dialogueClientCallbacks(Dialogue& dialogue)
{
    auto callbacks = WebSocket::Callbacks();

    callbacks.onOpen = [&dialogue](const std::string& protocol)
    {
        say("client: open, the server picked \"" + protocol + "\"");
        say("client: sending \"hello\"");
        dialogue.client->send("hello");
        say("client: sending 5 binary bytes");
        dialogue.client->sendBinary(std::string(5, '\x2a'));
    };

    callbacks.onMessage = [&dialogue](const WebSocket::Message& message)
    {
        say("client: heard " + described(message));

        if (--dialogue.repliesWanted == 0)
        {
            say("client: closing with 1000 \"done\"");
            dialogue.client->close(1000, "done");
        }
    };

    callbacks.onClose = [&dialogue](const WebSocket::CloseStatus& status)
    {
        say("client: closed with " + described(status));
        dialogue.clientClosed = true;
        endDialogueWhenBothEndsAreDone(dialogue);
    };

    callbacks.onError = [&dialogue](const std::string& error)
    {
        say("client: error: " + error);
        dialogue.failed = true;
        endDialogueWhenBothEndsAreDone(dialogue);
    };

    return callbacks;
}

int runBothEnds()
{
    auto dialogue = Dialogue();
    auto options = WebSocket::ServerOptions();
    options.protocols = {"demo"};

    auto server = WebSocket::Server {dialogueServerCallbacks(dialogue), options};
    dialogue.server = &server;

    if (!server.listen(0))
    {
        say("server: could not bind an ephemeral port");
        return 1;
    }

    auto address = "ws://127.0.0.1:" + std::to_string(server.boundPort()) + "/demo";
    say("server: listening on " + address);

    if (!clientIsSupported())
        return 1;

    auto clientOptions = WebSocket::Options();
    clientOptions.protocols = {"demo"};

    say("client: connecting to " + address);

    auto client = WebSocket::Connection {
        address, dialogueClientCallbacks(dialogue), clientOptions};
    dialogue.client = &client;

    auto over = [&dialogue] { return dialogueIsOver(dialogue); };
    auto ended = Threads::runEventLoopUntil(over, demoTimeout);
    server.stop();

    if (!ended)
    {
        say("timed out waiting for the dialogue to finish");
        return 1;
    }

    return dialogue.failed ? 1 : 0;
}

int runEchoServer(std::uint16_t port)
{
    WebSocket::Server* live = nullptr;
    auto connected = std::set<WebSocket::ClientId>();
    auto failed = false;
    auto callbacks = WebSocket::ServerCallbacks();

    callbacks.onConnect = [&live, &connected](WebSocket::ClientId id,
                                              const WebSocket::Handshake& handshake)
    {
        say("server: " + named(id) + " joined from " + handshake.peerHost
            + ", asking for " + handshake.path);

        for (auto other: connected)
            live->send(other, std::to_string(id) + " joined");

        connected.insert(id);
    };

    callbacks.onMessage =
        [&live](WebSocket::ClientId id, const WebSocket::Message& message)
    {
        say("server: echoing " + described(message) + " to " + named(id));

        if (message.type == WebSocket::MessageType::binary)
            live->sendBinary(id, message.data);
        else
            live->send(id, message.data);
    };

    callbacks.onDisconnect =
        [&live, &connected](WebSocket::ClientId id,
                            const WebSocket::CloseStatus& status)
    {
        say("server: " + named(id) + " left with " + described(status));
        connected.erase(id);
        live->broadcast(std::to_string(id) + " left");
    };

    callbacks.onError = [&failed](const std::string& error)
    {
        say("server: error " + error);
        failed = true;
        Threads::stopEventLoop();
    };

    auto server = WebSocket::Server {callbacks};
    live = &server;

    if (!server.listen(port))
    {
        say("server: could not listen on port " + std::to_string(port));
        return 1;
    }

    say("server: listening on ws://localhost:" + std::to_string(server.boundPort())
        + " (ctrl-c to stop)");

    Threads::runEventLoop();
    return failed ? 1 : 0;
}

int runOneMessageClient(const std::string& url, const std::string& message)
{
    if (!clientIsSupported())
        return 1;

    WebSocket::Connection* live = nullptr;
    auto failed = false;
    auto finished = false;
    auto closing = false;
    auto callbacks = WebSocket::Callbacks();

    callbacks.onOpen = [&live, &message](const std::string& protocol)
    {
        say("client: open, protocol \"" + protocol + "\"");
        say("client: sending \"" + message + "\"");
        live->send(message);
    };

    callbacks.onMessage = [&live, &closing](const WebSocket::Message& reply)
    {
        say("client: heard " + described(reply));

        if (closing)
            return;

        closing = true;
        say("client: closing with 1000 \"done\"");
        live->close(1000, "done");
    };

    callbacks.onClose = [&finished](const WebSocket::CloseStatus& status)
    {
        say("client: closed with " + described(status));
        finished = true;
        Threads::stopEventLoop();
    };

    callbacks.onError = [&failed](const std::string& error)
    {
        say("client: error: " + error);
        failed = true;
    };

    say("client: connecting to " + url);

    auto connection = WebSocket::Connection {url, callbacks};
    live = &connection;

    auto over = [&finished, &failed] { return finished || failed; };

    if (!Threads::runEventLoopUntil(over, demoTimeout))
    {
        say("client: timed out");
        return 1;
    }

    return failed ? 1 : 0;
}

std::uint16_t portFrom(const std::string& text)
{
    return (std::uint16_t) std::stoi(text);
}
} // namespace

int main(int argc, char** argv)
{
    auto arguments = std::vector<std::string>(argv + 1, argv + argc);

    if (arguments.empty())
        return runBothEnds();

    if (arguments[0] == "--serve" && arguments.size() <= 2)
        return runEchoServer(arguments.size() == 2 ? portFrom(arguments[1])
                                                   : std::uint16_t {8765});

    if (arguments[0] == "--connect" && arguments.size() >= 2
        && arguments.size() <= 3)
        return runOneMessageClient(arguments[1],
                                   arguments.size() == 3 ? arguments[2] : "hello");

    std::cerr << "usage: WebSocketDemo [--serve [PORT]"
                 " | --connect URL [MESSAGE]]\n";
    return 2;
}
