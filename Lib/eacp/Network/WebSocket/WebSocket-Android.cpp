#include "Backend.h"
#include "Protocol.h"

#include <eacp/Core/Android/Jni.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

// java.net.Socket through JNI, with Protocol.h doing all of the WebSocket: the
// NDK has no client, and the platform's sockets carry its TLS, certificate
// store and network security config.
namespace eacp::WebSocket
{

namespace
{

constexpr auto webSocketChunkSize = jsize {64 * 1024};
constexpr auto webSocketMaxHandshakeBytes = std::size_t {16 * 1024};
constexpr auto webSocketMaxFrameHeader = std::int64_t {14};
constexpr auto webSocketMaxCloseReason = std::size_t {123};

struct WebSocketJavaSockets
{
    void resolve(Jni::Lookup& java)
    {
        socket = java.findClass("java/net/Socket");
        socketAddress = java.findClass("java/net/InetSocketAddress");
        sslSocketFactory = java.findClass("javax/net/ssl/SSLSocketFactory");
        sslSocket = java.findClass("javax/net/ssl/SSLSocket");
        sslParameters = java.findClass("javax/net/ssl/SSLParameters");
        securityPolicy = java.findClass("android/security/NetworkSecurityPolicy");
        inputStream = java.findClass("java/io/InputStream");
        outputStream = java.findClass("java/io/OutputStream");

        socketInit = java.method(socket, "<init>", "()V");
        connect = java.method(socket, "connect", "(Ljava/net/SocketAddress;I)V");
        setTcpNoDelay = java.method(socket, "setTcpNoDelay", "(Z)V");
        setSoTimeout = java.method(socket, "setSoTimeout", "(I)V");
        getInputStream =
            java.method(socket, "getInputStream", "()Ljava/io/InputStream;");
        getOutputStream =
            java.method(socket, "getOutputStream", "()Ljava/io/OutputStream;");
        closeSocket = java.method(socket, "close", "()V");

        socketAddressInit =
            java.method(socketAddress, "<init>", "(Ljava/lang/String;I)V");

        defaultSslFactory = java.staticMethod(
            sslSocketFactory, "getDefault", "()Ljavax/net/SocketFactory;");
        createLayeredSocket =
            java.method(sslSocketFactory,
                        "createSocket",
                        "(Ljava/net/Socket;Ljava/lang/String;IZ)Ljava/net/Socket;");
        getSslParameters = java.method(
            sslSocket, "getSSLParameters", "()Ljavax/net/ssl/SSLParameters;");
        setSslParameters = java.method(
            sslSocket, "setSSLParameters", "(Ljavax/net/ssl/SSLParameters;)V");
        startHandshake = java.method(sslSocket, "startHandshake", "()V");
        setEndpointIdentification = java.method(sslParameters,
                                                "setEndpointIdentificationAlgorithm",
                                                "(Ljava/lang/String;)V");

        policyInstance =
            java.staticMethod(securityPolicy,
                              "getInstance",
                              "()Landroid/security/NetworkSecurityPolicy;");
        cleartextPermitted = java.method(
            securityPolicy, "isCleartextTrafficPermitted", "(Ljava/lang/String;)Z");

        read = java.method(inputStream, "read", "([B)I");
        write = java.method(outputStream, "write", "([BII)V");
    }

    jclass socket = nullptr;
    jclass socketAddress = nullptr;
    jclass sslSocketFactory = nullptr;
    jclass sslSocket = nullptr;
    jclass sslParameters = nullptr;
    jclass securityPolicy = nullptr;
    jclass inputStream = nullptr;
    jclass outputStream = nullptr;

    jmethodID socketInit = nullptr;
    jmethodID connect = nullptr;
    jmethodID setTcpNoDelay = nullptr;
    jmethodID setSoTimeout = nullptr;
    jmethodID getInputStream = nullptr;
    jmethodID getOutputStream = nullptr;
    jmethodID closeSocket = nullptr;
    jmethodID socketAddressInit = nullptr;
    jmethodID defaultSslFactory = nullptr;
    jmethodID createLayeredSocket = nullptr;
    jmethodID getSslParameters = nullptr;
    jmethodID setSslParameters = nullptr;
    jmethodID startHandshake = nullptr;
    jmethodID setEndpointIdentification = nullptr;
    jmethodID policyInstance = nullptr;
    jmethodID cleartextPermitted = nullptr;
    jmethodID read = nullptr;
    jmethodID write = nullptr;
};

void webSocketCheck(JNIEnv* env)
{
    auto message = Jni::takeException(env);

    if (!message.empty())
        throw std::runtime_error(message);
}

jint webSocketMilliseconds(Time::MS timeout)
{
    auto maxMilliseconds = (std::int64_t) std::numeric_limits<jint>::max();
    return (jint) std::clamp(timeout.count, std::int64_t {0}, maxMilliseconds);
}

Protocol::Frame webSocketCloseFrame(int code, const std::string& reason)
{
    auto fitting = std::string_view {reason}.substr(0, webSocketMaxCloseReason);
    return {Protocol::Opcode::close, true, Protocol::encodeClose(code, fitting)};
}

// One connection's state, shared by its two threads and the Backend that
// fronts it, so whichever of them goes last frees it. The reader connects,
// shakes hands, then decodes frames until the conversation ends; the writer
// drains a queue of frames already encoded and masked, so nothing on the
// message thread waits on the network. A local reference is good only on the
// thread that made it, so what more than one thread touches - the sockets, to
// close them, and the stream the writer writes - is held as a global one.
class WebSocketAndroidTransport
{
public:
    WebSocketAndroidTransport(const Protocol::Address& addressToUse,
                              const Options& optionsToUse,
                              const std::shared_ptr<Sink>& sinkToUse,
                              const WebSocketJavaSockets& javaToUse)
        : address(addressToUse)
        , options(optionsToUse)
        , sink(sinkToUse)
        , java(javaToUse)
        , key(Protocol::randomClientKey())
    {
    }

    WebSocketAndroidTransport(const WebSocketAndroidTransport&) = delete;
    WebSocketAndroidTransport& operator=(const WebSocketAndroidTransport&) = delete;

    void runReader()
    {
        auto* env = Jni::currentEnv();

        if (env == nullptr)
            reportFailed("No Java VM to connect through");
        else
            connectAndReceive(env);

        finishThread(env, true);
    }

    void runWriter()
    {
        auto* env = Jni::currentEnv();

        if (env != nullptr)
            writeQueuedFrames(env);

        finishThread(env, false);
    }

    void send(const Message& message)
    {
        auto opcode = message.type == MessageType::binary ? Protocol::Opcode::binary
                                                          : Protocol::Opcode::text;
        {
            auto lock = std::scoped_lock(mutex);

            if (!isOpen || closeRequested || stopping)
                return;

            outbound.push_back(Protocol::encode({opcode, true, message.data}, true));
        }

        wake.notify_all();
    }

    void close(int code, const std::string& reason)
    {
        auto lock = std::unique_lock(mutex);

        if (closeRequested)
            return;

        closeRequested = true;

        if (!isOpen)
        {
            lock.unlock();
            closePlainSocket();
            return;
        }

        outbound.push_back(
            Protocol::encode(webSocketCloseFrame(code, reason), true));
        lock.unlock();
        wake.notify_all();
    }

    // The Backend has gone: nothing more is reported, and closing the plain
    // socket throws out of whatever call either thread is blocked in.
    void abandon()
    {
        {
            auto lock = std::scoped_lock(mutex);
            stopping = true;
            outbound.clear();
        }

        wake.notify_all();
        closePlainSocket();
    }

private:
    void connectAndReceive(JNIEnv* env)
    {
        auto frame = Jni::LocalFrame {env, 64};
        auto* buffer = env->NewByteArray(webSocketChunkSize);
        auto pending = std::string();
        auto* input = static_cast<jobject>(nullptr);

        try
        {
            webSocketCheck(env);
            input = openConnection(env, buffer, pending);
        }
        catch (const std::exception& e)
        {
            reportHandshakeEnd(e.what());
            return;
        }

        sink->opened(chosenProtocol);
        receive(env, input, buffer, pending);
    }

    jobject openConnection(JNIEnv* env, jbyteArray buffer, std::string& pending)
    {
        auto* host = Jni::toJava(env, std::string_view {address.host});
        webSocketCheck(env);

        throwIfCleartextRefused(env, host);

        auto* plain = env->NewObject(java.socket, java.socketInit);
        webSocketCheck(env);
        adopt(env, plainSocket, plain);

        auto* endpoint = env->NewObject(
            java.socketAddress, java.socketAddressInit, host, address.port);
        webSocketCheck(env);

        auto timeout = webSocketMilliseconds(options.connectTimeout);
        env->CallVoidMethod(plain, java.connect, endpoint, timeout);
        webSocketCheck(env);
        env->CallVoidMethod(plain, java.setTcpNoDelay, JNI_TRUE);
        env->CallVoidMethod(plain, java.setSoTimeout, timeout);
        webSocketCheck(env);

        auto* socket = address.secure ? secureLayer(env, plain, host) : plain;
        adopt(env, topSocket, socket);

        auto* input = env->CallObjectMethod(socket, java.getInputStream);
        webSocketCheck(env);
        auto* output = env->CallObjectMethod(socket, java.getOutputStream);
        webSocketCheck(env);

        writeAll(env,
                 output,
                 buffer,
                 Protocol::clientHandshakeRequest(address, key, options));
        pending = readHandshake(env, input, buffer);

        env->CallVoidMethod(plain, java.setSoTimeout, 0);
        webSocketCheck(env);

        publishOpen(env, output);
        return input;
    }

    void throwIfCleartextRefused(JNIEnv* env, jstring host)
    {
        if (address.secure)
            return;

        auto* policy =
            env->CallStaticObjectMethod(java.securityPolicy, java.policyInstance);
        webSocketCheck(env);

        auto permitted =
            env->CallBooleanMethod(policy, java.cleartextPermitted, host);
        webSocketCheck(env);

        if (!permitted)
            throw std::runtime_error("The app's network security config refuses "
                                     "cleartext ws:// to "
                                     + address.host);
    }

    // §4.1 asks the client to check the server's certificate against the host
    // it meant, which an SSLSocket made by hand leaves off; the host given to
    // createSocket is also what the platform puts in SNI.
    jobject secureLayer(JNIEnv* env, jobject plain, jstring host)
    {
        auto* factory = env->CallStaticObjectMethod(java.sslSocketFactory,
                                                    java.defaultSslFactory);
        webSocketCheck(env);

        auto* secure = env->CallObjectMethod(
            factory, java.createLayeredSocket, plain, host, address.port, JNI_TRUE);
        webSocketCheck(env);

        auto* parameters = env->CallObjectMethod(secure, java.getSslParameters);
        webSocketCheck(env);

        auto* algorithm = Jni::toJava(env, std::string_view {"HTTPS"});
        webSocketCheck(env);

        env->CallVoidMethod(parameters, java.setEndpointIdentification, algorithm);
        webSocketCheck(env);
        env->CallVoidMethod(secure, java.setSslParameters, parameters);
        webSocketCheck(env);
        env->CallVoidMethod(secure, java.startHandshake);
        webSocketCheck(env);

        return secure;
    }

    // Published under the lock that close() and abandon() take, so either
    // they see the socket and close it, or this sees them and gives up.
    void adopt(JNIEnv* env, Jni::GlobalRef& slot, jobject socket)
    {
        auto lock = std::scoped_lock(mutex);

        if (stopping || closeRequested)
            throw std::runtime_error("The connection was abandoned");

        slot.reset(env, socket);
    }

    void publishOpen(JNIEnv* env, jobject output)
    {
        auto lock = std::scoped_lock(mutex);

        if (stopping || closeRequested)
            throw std::runtime_error("The connection was abandoned");

        outputStream.reset(env, output);
        isOpen = true;
    }

    // Whatever arrives after the blank line is already the frame stream.
    std::string readHandshake(JNIEnv* env, jobject input, jbyteArray buffer)
    {
        auto received = std::string();

        while (true)
        {
            auto end = received.find("\r\n\r\n");

            if (end != std::string::npos)
                return acceptHandshake(received, end + 4);

            if (received.size() > webSocketMaxHandshakeBytes)
                throw std::runtime_error(
                    "The server's handshake answer is too long");

            if (!readChunk(env, input, buffer, received))
                throw std::runtime_error(
                    "The server closed the connection during the handshake");
        }
    }

    std::string acceptHandshake(const std::string& received, std::size_t headEnd)
    {
        auto head = std::string_view {received}.substr(0, headEnd);
        auto result =
            Protocol::validateHandshakeResponse(head, key, options.protocols);

        if (!result.ok)
            throw std::runtime_error(result.error);

        chosenProtocol = result.protocol;
        return received.substr(headEnd);
    }

    // Appends what one read brought; false at the end of the stream.
    bool readChunk(JNIEnv* env, jobject input, jbyteArray buffer, std::string& into)
    {
        auto count = env->CallIntMethod(input, java.read, buffer);
        webSocketCheck(env);

        if (count < 0)
            return false;

        auto start = into.size();
        into.resize(start + (std::size_t) count);
        env->GetByteArrayRegion(
            buffer, 0, count, reinterpret_cast<jbyte*>(into.data() + start));
        webSocketCheck(env);

        return true;
    }

    void writeAll(JNIEnv* env,
                  jobject output,
                  jbyteArray buffer,
                  std::string_view bytes)
    {
        while (!bytes.empty())
        {
            auto count =
                (jsize) std::min<std::size_t>(bytes.size(), webSocketChunkSize);

            env->SetByteArrayRegion(
                buffer, 0, count, reinterpret_cast<const jbyte*>(bytes.data()));
            env->CallVoidMethod(output, java.write, buffer, 0, count);
            webSocketCheck(env);

            bytes.remove_prefix((std::size_t) count);
        }
    }

    void receive(JNIEnv* env, jobject input, jbyteArray buffer, std::string& pending)
    {
        while (drainFrames(pending))
        {
            auto frameForRead = Jni::LocalFrame {env, 8};
            auto more = false;

            try
            {
                more = readChunk(env, input, buffer, pending);
            }
            catch (const std::exception& e)
            {
                reportTransportEnd(e.what());
                return;
            }

            if (!more)
            {
                reportTransportEnd("The server closed the connection");
                return;
            }
        }
    }

    // Decodes every whole frame; false once the conversation is over. What
    // is left is one partial frame, which may not outgrow the limit either.
    bool drainFrames(std::string& pending)
    {
        try
        {
            while (auto decoded = Protocol::decode(pending))
            {
                pending.erase(0, (std::size_t) decoded->consumed);

                if (!handleFrame(decoded->frame))
                    return false;
            }
        }
        catch (const Protocol::Error& e)
        {
            return failWithClose(1002, e.what());
        }

        auto partial = (std::int64_t) (assembled.size() + pending.size());

        if (partial > options.maxMessageSize + webSocketMaxFrameHeader)
            return rejectOversized();

        return true;
    }

    bool handleFrame(const Protocol::Frame& frame)
    {
        switch (frame.opcode)
        {
            case Protocol::Opcode::ping:
                queueControl({Protocol::Opcode::pong, true, frame.payload});
                return true;

            case Protocol::Opcode::pong:
                return true;

            case Protocol::Opcode::close:
                return handleClose(frame.payload);

            default:
                return handleData(frame);
        }
    }

    bool handleData(const Protocol::Frame& frame)
    {
        auto continues = frame.opcode == Protocol::Opcode::continuation;

        if (continues != messageInProgress)
            return failWithClose(1002, "A fragment out of sequence");

        if (!continues)
        {
            messageInProgress = true;
            messageType = frame.opcode == Protocol::Opcode::binary
                              ? MessageType::binary
                              : MessageType::text;
        }

        auto size = (std::int64_t) (assembled.size() + frame.payload.size());

        if (size > options.maxMessageSize)
            return rejectOversized();

        assembled += frame.payload;

        if (!frame.fin)
            return true;

        messageInProgress = false;
        sink->received({std::exchange(assembled, {}), messageType});
        return true;
    }

    bool handleClose(const std::string& payload)
    {
        auto status = CloseStatus();

        try
        {
            status = Protocol::decodeClose(payload);
        }
        catch (const Protocol::Error& e)
        {
            return failWithClose(1002, e.what());
        }

        queueCloseUnlessSent({Protocol::Opcode::close, true, payload});
        reportClosed(status.code, status.reason);
        return false;
    }

    bool rejectOversized()
    {
        queueCloseUnlessSent(webSocketCloseFrame(1009, "message too big"));
        reportFailed("message exceeds maxMessageSize");
        return false;
    }

    bool failWithClose(int code, const std::string& error)
    {
        queueCloseUnlessSent(webSocketCloseFrame(code, {}));
        reportFailed(error);
        return false;
    }

    void queueControl(const Protocol::Frame& frame)
    {
        {
            auto lock = std::scoped_lock(mutex);
            outbound.push_back(Protocol::encode(frame, true));
        }

        wake.notify_all();
    }

    void queueCloseUnlessSent(const Protocol::Frame& frame)
    {
        {
            auto lock = std::scoped_lock(mutex);

            if (closeRequested)
                return;

            closeRequested = true;
            outbound.push_back(Protocol::encode(frame, true));
        }

        wake.notify_all();
    }

    struct Outgoing
    {
        std::string bytes;
        jobject output = nullptr;
    };

    // Waits for the next frame; nullopt once the writer has nothing left to
    // do - the Backend gone, or the reader done and the queue drained.
    std::optional<Outgoing> takeNextFrame()
    {
        auto lock = std::unique_lock(mutex);
        auto hasWork = [this]
        { return stopping || readerDone || !outbound.empty(); };
        wake.wait(lock, hasWork);

        if (stopping || outbound.empty())
            return std::nullopt;

        auto next =
            Outgoing {std::exchange(outbound.front(), {}), outputStream.get()};
        outbound.pop_front();
        return next;
    }

    void writeQueuedFrames(JNIEnv* env)
    {
        auto frame = Jni::LocalFrame {env, 8};
        auto* buffer = env->NewByteArray(webSocketChunkSize);

        if (buffer == nullptr)
        {
            noteWriteFailure(Jni::takeException(env));
            return;
        }

        while (auto next = takeNextFrame())
        {
            auto frameForWrite = Jni::LocalFrame {env, 8};

            try
            {
                writeAll(env, next->output, buffer, next->bytes);
            }
            catch (const std::exception& e)
            {
                noteWriteFailure(e.what());
                return;
            }
        }
    }

    // The reader is blocked in a read that would never learn of it, so the
    // socket goes, and the reader reports this error rather than its own.
    void noteWriteFailure(const std::string& error)
    {
        {
            auto lock = std::scoped_lock(mutex);
            writeError = error;
        }

        closePlainSocket();
    }

    void closePlainSocket()
    {
        auto* env = Jni::currentEnv();

        if (env == nullptr)
            return;

        auto* socket = socketIn(plainSocket);

        if (socket == nullptr)
            return;

        env->CallVoidMethod(socket, java.closeSocket);
        env->ExceptionClear();
    }

    // The last thread out closes the TLS layer first, so a peer still
    // listening gets its close_notify, then the socket under it.
    void finishThread(JNIEnv* env, bool isReader)
    {
        auto isLast = false;

        {
            auto lock = std::scoped_lock(mutex);
            readerDone = readerDone || isReader;
            isLast = --runningThreads == 0;
        }

        wake.notify_all();

        if (!isLast || env == nullptr)
            return;

        for (auto* socket: {socketIn(topSocket), socketIn(plainSocket)})
        {
            if (socket == nullptr)
                continue;

            env->CallVoidMethod(socket, java.closeSocket);
            env->ExceptionClear();
        }
    }

    jobject socketIn(const Jni::GlobalRef& slot)
    {
        auto lock = std::scoped_lock(mutex);
        return slot.get();
    }

    void reportHandshakeEnd(const std::string& error)
    {
        auto lock = std::unique_lock(mutex);
        auto abandoned = stopping;
        auto closing = closeRequested;
        lock.unlock();

        if (abandoned)
            return;

        if (closing)
            reportClosed(1006, {});
        else
            reportFailed(error);
    }

    void reportTransportEnd(const std::string& error)
    {
        auto lock = std::unique_lock(mutex);
        auto abandoned = stopping;
        auto closing = closeRequested;
        auto cause = writeError.empty() ? error : writeError;
        lock.unlock();

        if (abandoned)
            return;

        if (closing)
            reportClosed(1006, {});
        else
            reportFailed(cause);
    }

    void reportClosed(int code, const std::string& reason)
    {
        if (!terminalReported.exchange(true))
            sink->closed(code, reason);
    }

    void reportFailed(const std::string& error)
    {
        if (!terminalReported.exchange(true))
            sink->failed(error);
    }

    Protocol::Address address;
    Options options;
    std::shared_ptr<Sink> sink;
    const WebSocketJavaSockets& java;
    std::string key;

    std::string chosenProtocol;
    std::string assembled;
    MessageType messageType = MessageType::text;
    bool messageInProgress = false;
    std::atomic<bool> terminalReported {false};

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> outbound;
    std::string writeError;
    Jni::GlobalRef plainSocket;
    Jni::GlobalRef topSocket;
    Jni::GlobalRef outputStream;
    int runningThreads = 2;
    bool isOpen = false;
    bool closeRequested = false;
    bool readerDone = false;
    bool stopping = false;
};

// The threads hold the transport as well, and are let go rather than joined:
// a host name still resolving inside the InetSocketAddress constructor answers
// no close, and the destructor waits on no network.
class WebSocketAndroidBackend final : public Backend
{
public:
    explicit WebSocketAndroidBackend(
        const std::shared_ptr<WebSocketAndroidTransport>& transportToUse)
        : transport(transportToUse)
    {
        auto readFrames = [transportToUse] { transportToUse->runReader(); };
        auto writeFrames = [transportToUse] { transportToUse->runWriter(); };

        std::thread(readFrames).detach();
        std::thread(writeFrames).detach();
    }

    ~WebSocketAndroidBackend() override { transport->abandon(); }

    void send(const Message& message) override { transport->send(message); }

    void close(int code, const std::string& reason) override
    {
        transport->close(code, reason);
    }

private:
    std::shared_ptr<WebSocketAndroidTransport> transport;
};

class WebSocketMissingBackend final : public Backend
{
public:
    WebSocketMissingBackend(const std::shared_ptr<Sink>& sink,
                            const std::string& error)
    {
        sink->failed(error);
    }

    void send(const Message&) override {}
    void close(int, const std::string&) override {}
};

const WebSocketJavaSockets* webSocketJava(JNIEnv* env)
{
    return env != nullptr ? Jni::resolveOnce<WebSocketJavaSockets>(env) : nullptr;
}

} // namespace

bool backendIsSupported()
{
    return webSocketJava(Jni::currentEnv()) != nullptr;
}

std::unique_ptr<Backend> makeBackend(const std::string& url,
                                     const Options& options,
                                     std::shared_ptr<Sink> sink)
{
    const auto* java = webSocketJava(Jni::currentEnv());

    if (java == nullptr)
        return std::make_unique<WebSocketMissingBackend>(
            sink, "java.net.Socket is unavailable to connect through");

    auto address = Protocol::parseUrl(url);

    if (!address.has_value())
        return std::make_unique<WebSocketMissingBackend>(
            sink, "Not a ws:// or wss:// URL: " + url);

    auto transport =
        std::make_shared<WebSocketAndroidTransport>(*address, options, sink, *java);

    return std::make_unique<WebSocketAndroidBackend>(transport);
}

} // namespace eacp::WebSocket
