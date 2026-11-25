#pragma once

#include "../Common.h"

#include <map>

namespace eacp::WebSocket
{

enum class MessageType
{
    text,
    binary,
};

struct Message
{
    std::string data;
    MessageType type = MessageType::text;
};

// RFC 6455 §7.4: 1000 normal, 1005 no status received, 1006 abnormal closure
struct CloseStatus
{
    int code = 1005;
    std::string reason;
};

enum class State
{
    connecting,
    open,
    closing,
    closed,
};

// closeTimeout bounds the closing handshake: a peer that answers close()
// with neither a close frame nor an end of stream is dropped, as 1006.
//
// On the web the connection is the runtime's own WebSocket, which takes no
// headers from a script, so a connection given any fails as it opens;
// maxMessageSize is checked once a message has arrived whole, since the
// runtime reassembles it first; and text is sent as a string, so text that is
// not valid UTF-8 goes out with each invalid sequence replaced by U+FFFD.
struct Options
{
    std::map<std::string, std::string> headers;
    Vector<std::string> protocols;
    Time::MS connectTimeout {15000};
    Time::MS closeTimeout {5000};
    int maxMessageSize = 64 * 1024 * 1024;
};

// Every callback runs on the message thread, none inside a Connection call
// and none once the Connection is destroyed. onError comes at most once and
// is followed by onClose with code 1006.
struct Callbacks
{
    std::function<void(const std::string& protocol)> onOpen =
        [](const std::string&) {};
    std::function<void(const Message&)> onMessage = [](const Message&) {};
    std::function<void(const CloseStatus&)> onClose = [](const CloseStatus&) {};
    std::function<void(const std::string& error)> onError =
        [](const std::string&) {};
};

// A client connection: constructing one starts the handshake, destroying it
// closes the socket. Used from the message thread only.
class Connection
{
public:
    Connection(const std::string& url,
               Callbacks callbacks,
               const Options& options = {});
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // False where the platform's HTTP stack cannot speak the protocol - a
    // libcurl built without it. Constructing one there fails through onError.
    static bool isSupported();

    // Dropped unless the connection is open
    void send(std::string_view text);
    void sendBinary(std::string_view bytes);

    // Starts the closing handshake. Before open it abandons the attempt,
    // reporting onClose with 1006 and no onError. A browser lets a script
    // send only 1000 and 3000-4999, so on the web any other code goes as 1000.
    void close(int code = 1000, std::string_view reason = {});

    [[nodiscard]] State state() const;
    [[nodiscard]] const std::string& url() const;

    // The subprotocol the server picked, empty until open
    [[nodiscard]] const std::string& protocol() const;

private:
    struct Impl;
    OwningPointer<Impl> impl;
};

} // namespace eacp::WebSocket
