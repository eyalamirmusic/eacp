#pragma once

#include <eacp/Network/Network.h>
#include <eacp/Network/WebSocket/Protocol.h>

#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// What the server does once a client is on the far end. Everything here is a
// scenario some test needs and no live server would ever do on purpose.
struct WebSocketTestServerOptions
{
    // Answer the upgrade with a 404 instead of a 101.
    bool rejectHandshake = false;

    // Accept the TCP connection and then say nothing at all.
    bool stall = false;

    // A message pushed the moment the handshake is answered.
    std::string greeting;

    // Send that greeting as three continuation-linked frames.
    bool fragmentGreeting = false;

    bool pingAfterHandshake = false;

    bool closeAfterHandshake = false;
    int closeCode = 1000;
    std::string closeReason;

    // Take the client's close frame, answer neither it nor anything else, and
    // hold the socket open until release() - the peer a closing handshake has
    // to be bounded against.
    bool stallOnClose = false;
};

// A single-threaded RFC 6455 server built on TCP::Listener and Protocol.h -
// enough of one to prove a client speaks the protocol, and no more. It serves
// one connection at a time, records what it saw for the test to assert on,
// and works to short timeouts so a wedged case fails in seconds.
class WebSocketTestServer
{
public:
    using Frame = eacp::WebSocket::Protocol::Frame;
    using Opcode = eacp::WebSocket::Protocol::Opcode;
    using Peer = eacp::TCP::Connection;

    explicit WebSocketTestServer(WebSocketTestServerOptions optionsToUse = {})
        : options(std::move(optionsToUse))
        , listener(eacp::TCP::Listener::bind(0, serverTimeouts()))
        , boundPort(listener.port())
    {
        worker = std::thread([this] { run(); });
    }

    ~WebSocketTestServer()
    {
        stopping.store(true);

        if (worker.joinable())
            worker.join();
    }

    WebSocketTestServer(const WebSocketTestServer&) = delete;
    WebSocketTestServer& operator=(const WebSocketTestServer&) = delete;

    std::string url() const { return "ws://127.0.0.1:" + std::to_string(boundPort); }

    // Header names are matched case-insensitively, as HTTP spells them.
    std::string requestHeader(const std::string& name) const
    {
        auto lock = std::scoped_lock(mutex);
        auto found = requestHeaders.find(lowercased(name));
        return found != requestHeaders.end() ? found->second : std::string();
    }

    std::vector<std::string> messages() const
    {
        auto lock = std::scoped_lock(mutex);
        return received;
    }

    std::size_t messageCount() const
    {
        auto lock = std::scoped_lock(mutex);
        return received.size();
    }

    bool sawHandshake() const { return handshakeDone.load(); }
    bool sawPong() const { return pongSeen.load(); }
    bool sawClose() const { return closeSeen.load(); }
    bool sawDisconnect() const { return disconnectSeen.load(); }

    // The client's own end of the stream, as opposed to a frame loop that
    // ended for any other reason.
    bool sawPeerEnd() const { return peerEndSeen.load(); }

    // Lets a stalled connection go, so a test need not wait out teardown.
    void release() { released.store(true); }

    eacp::WebSocket::CloseStatus closeStatus() const
    {
        auto lock = std::scoped_lock(mutex);
        return closeFromClient;
    }

private:
    static eacp::TCP::Timeouts serverTimeouts()
    {
        return {eacp::Time::MS {200}, eacp::Time::MS {300}};
    }

    static std::string lowercased(std::string text)
    {
        for (auto& character: text)
            character = (char) std::tolower((unsigned char) character);

        return text;
    }

    static std::string trimmed(std::string_view text)
    {
        auto start = text.find_first_not_of(" \t");

        if (start == std::string_view::npos)
            return {};

        auto end = text.find_last_not_of(" \t");
        return std::string(text.substr(start, end - start + 1));
    }

    void run()
    {
        while (!stopping.load())
        {
            auto peer = std::optional<Peer>();

            try
            {
                peer = listener.accept();
            }
            catch (const eacp::TCP::Error&)
            {
                continue;
            }

            try
            {
                serve(*peer);
            }
            catch (const std::exception&)
            {
                noteDisconnect();
            }
        }
    }

    void serve(Peer& peer)
    {
        assembly.clear();
        assemblyOpcode = Opcode::text;
        closeSent = false;

        if (options.stall)
        {
            while (!stopping.load())
                eacp::Time::sleepMS(20);

            return;
        }

        if (!readRequestHead(peer))
            return;

        if (options.rejectHandshake)
        {
            peer.send("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                      "Connection: close\r\n\r\n");
            return;
        }

        if (!isWebSocketUpgrade())
            return;

        peer.send(handshakeResponse());
        handshakeDone.store(true);

        afterHandshake(peer);
        frameLoop(peer);
        noteDisconnect();
    }

    bool readRequestHead(Peer& peer)
    {
        auto headers = std::map<std::string, std::string>();

        for (auto line = 0; line < 64; ++line)
        {
            auto text = std::string();

            try
            {
                text = peer.receiveLine();
            }
            catch (const eacp::TCP::Error&)
            {
                return false;
            }

            if (text.empty())
            {
                auto lock = std::scoped_lock(mutex);
                requestHeaders = std::move(headers);
                return true;
            }

            auto colon = text.find(':');

            if (colon != std::string::npos)
                headers[lowercased(trimmed(text.substr(0, colon)))] =
                    trimmed(text.substr(colon + 1));
        }

        return false;
    }

    bool isWebSocketUpgrade() const
    {
        return lowercased(requestHeader("upgrade")) == "websocket"
               && requestHeader("sec-websocket-version") == "13"
               && !requestHeader("sec-websocket-key").empty();
    }

    std::string firstOfferedProtocol() const
    {
        auto offered = requestHeader("sec-websocket-protocol");

        if (offered.empty())
            return {};

        auto comma = offered.find(',');
        return trimmed(comma == std::string::npos ? offered
                                                  : offered.substr(0, comma));
    }

    std::string handshakeResponse() const
    {
        auto key = requestHeader("sec-websocket-key");

        auto response = std::string("HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n");

        response += "Sec-WebSocket-Accept: "
                    + eacp::WebSocket::Protocol::acceptKeyFor(key) + "\r\n";

        auto chosen = firstOfferedProtocol();

        if (!chosen.empty())
            response += "Sec-WebSocket-Protocol: " + chosen + "\r\n";

        return response + "\r\n";
    }

    void afterHandshake(Peer& peer)
    {
        if (!options.greeting.empty())
            sendGreeting(peer);

        if (options.pingAfterHandshake)
            sendFrame(peer, {Opcode::ping, true, "server-ping"});

        if (options.closeAfterHandshake)
        {
            auto payload = eacp::WebSocket::Protocol::encodeClose(
                options.closeCode, options.closeReason);

            sendFrame(peer, {Opcode::close, true, payload});
            closeSent = true;
        }
    }

    void sendGreeting(Peer& peer)
    {
        if (!options.fragmentGreeting)
        {
            sendFrame(peer, {Opcode::text, true, options.greeting});
            return;
        }

        auto third = options.greeting.size() / 3;

        sendFrame(peer, {Opcode::text, false, options.greeting.substr(0, third)});
        sendFrame(
            peer,
            {Opcode::continuation, false, options.greeting.substr(third, third)});
        sendFrame(peer,
                  {Opcode::continuation, true, options.greeting.substr(2 * third)});
    }

    void frameLoop(Peer& peer)
    {
        auto buffer = std::string();

        while (!stopping.load())
        {
            auto chunk = std::string();

            try
            {
                chunk = peer.receive(65536);
            }
            catch (const eacp::TCP::TimeoutError&)
            {
                continue;
            }
            catch (const eacp::TCP::Error&)
            {
                notePeerEnd();
                return;
            }

            if (chunk.empty())
            {
                notePeerEnd();
                return;
            }

            buffer += chunk;

            if (!drainFrames(peer, buffer))
                return;
        }
    }

    bool drainFrames(Peer& peer, std::string& buffer)
    {
        while (true)
        {
            auto decoded = std::optional<eacp::WebSocket::Protocol::Decoded>();

            try
            {
                decoded = eacp::WebSocket::Protocol::decode(buffer);
            }
            catch (const eacp::WebSocket::Protocol::Error&)
            {
                noteDisconnect();
                return false;
            }

            if (!decoded.has_value())
                return true;

            buffer.erase(0, decoded->consumed);

            if (!handleFrame(peer, decoded->frame))
                return false;
        }
    }

    bool handleFrame(Peer& peer, const Frame& frame)
    {
        switch (frame.opcode)
        {
            case Opcode::continuation:
                assembly += frame.payload;

                if (frame.fin)
                {
                    echo(peer, assemblyOpcode, assembly);
                    assembly.clear();
                }

                return true;

            case Opcode::text:
            case Opcode::binary:
                if (!frame.fin)
                {
                    assemblyOpcode = frame.opcode;
                    assembly = frame.payload;
                    return true;
                }

                echo(peer, frame.opcode, frame.payload);
                return true;

            case Opcode::ping:
                sendFrame(peer, {Opcode::pong, true, frame.payload});
                return true;

            case Opcode::pong:
                pongSeen.store(true);
                return true;

            case Opcode::close:
                noteClose(frame.payload);

                if (options.stallOnClose)
                    return stallHoldingTheSocket(peer);

                if (!closeSent)
                    sendFrame(peer, {Opcode::close, true, frame.payload});

                return false;
        }

        return true;
    }

    // Says nothing back and closes nothing, but keeps reading, so the client
    // giving up on the handshake still shows as the end of the stream it is.
    bool stallHoldingTheSocket(Peer& peer)
    {
        while (!stopping.load() && !released.load())
        {
            try
            {
                if (peer.receive(1024).empty())
                {
                    notePeerEnd();
                    return false;
                }
            }
            catch (const eacp::TCP::TimeoutError&)
            {
                continue;
            }
            catch (const eacp::TCP::Error&)
            {
                notePeerEnd();
                return false;
            }
        }

        return false;
    }

    void echo(Peer& peer, Opcode opcode, const std::string& payload)
    {
        {
            auto lock = std::scoped_lock(mutex);
            received.push_back(payload);
        }

        sendFrame(peer, {opcode, true, payload});
    }

    void sendFrame(Peer& peer, const Frame& frame)
    {
        peer.send(eacp::WebSocket::Protocol::encode(frame, false));
    }

    void noteClose(const std::string& payload)
    {
        auto status = eacp::WebSocket::CloseStatus();

        try
        {
            status = eacp::WebSocket::Protocol::decodeClose(payload);
        }
        catch (const eacp::WebSocket::Protocol::Error&)
        {
            status = {1002, {}};
        }

        {
            auto lock = std::scoped_lock(mutex);
            closeFromClient = status;
        }

        closeSeen.store(true);
    }

    void notePeerEnd()
    {
        peerEndSeen.store(true);
        noteDisconnect();
    }

    void noteDisconnect() { disconnectSeen.store(true); }

    WebSocketTestServerOptions options;
    eacp::TCP::Listener listener;
    std::uint16_t boundPort = 0;

    mutable std::mutex mutex;
    std::map<std::string, std::string> requestHeaders;
    std::vector<std::string> received;
    eacp::WebSocket::CloseStatus closeFromClient;

    std::atomic<bool> stopping {false};
    std::atomic<bool> handshakeDone {false};
    std::atomic<bool> pongSeen {false};
    std::atomic<bool> closeSeen {false};
    std::atomic<bool> disconnectSeen {false};
    std::atomic<bool> peerEndSeen {false};
    std::atomic<bool> released {false};

    std::string assembly;
    Opcode assemblyOpcode = Opcode::text;
    bool closeSent = false;

    std::thread worker;
};
