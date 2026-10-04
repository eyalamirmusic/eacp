#include "Http.h"
#include "HttpProtocol.h"

#include <eacp/Core/Android/Jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

// java.net.HttpURLConnection through JNI: the NDK has no HTTP client, and this
// one carries the platform's TLS, certificate store, proxy settings and
// network security config.
namespace eacp::HTTP
{

namespace
{

constexpr auto chunkSize = jsize {64 * 1024};

struct JavaNet
{
    void resolve(Jni::Lookup& java)
    {
        url = java.findClass("java/net/URL");
        connection = java.findClass("java/net/HttpURLConnection");
        inputStream = java.findClass("java/io/InputStream");
        outputStream = java.findClass("java/io/OutputStream");

        urlInit = java.method(url, "<init>", "(Ljava/lang/String;)V");
        openConnection =
            java.method(url, "openConnection", "()Ljava/net/URLConnection;");

        setRequestMethod =
            java.method(connection, "setRequestMethod", "(Ljava/lang/String;)V");
        setRequestProperty = java.method(connection,
                                         "setRequestProperty",
                                         "(Ljava/lang/String;Ljava/lang/String;)V");
        setDoOutput = java.method(connection, "setDoOutput", "(Z)V");
        setFixedLengthStreamingMode =
            java.method(connection, "setFixedLengthStreamingMode", "(J)V");
        setInstanceFollowRedirects =
            java.method(connection, "setInstanceFollowRedirects", "(Z)V");
        setUseCaches = java.method(connection, "setUseCaches", "(Z)V");
        setConnectTimeout = java.method(connection, "setConnectTimeout", "(I)V");
        setReadTimeout = java.method(connection, "setReadTimeout", "(I)V");
        getOutputStream =
            java.method(connection, "getOutputStream", "()Ljava/io/OutputStream;");
        getResponseCode = java.method(connection, "getResponseCode", "()I");
        getHeaderFieldKey =
            java.method(connection, "getHeaderFieldKey", "(I)Ljava/lang/String;");
        getHeaderField =
            java.method(connection, "getHeaderField", "(I)Ljava/lang/String;");
        getContentLengthLong =
            java.method(connection, "getContentLengthLong", "()J");
        getInputStream =
            java.method(connection, "getInputStream", "()Ljava/io/InputStream;");
        getErrorStream =
            java.method(connection, "getErrorStream", "()Ljava/io/InputStream;");
        disconnect = java.method(connection, "disconnect", "()V");

        read = java.method(inputStream, "read", "([B)I");
        closeInput = java.method(inputStream, "close", "()V");
        write = java.method(outputStream, "write", "([B)V");
        closeOutput = java.method(outputStream, "close", "()V");
    }

    jclass url = nullptr;
    jclass connection = nullptr;
    jclass inputStream = nullptr;
    jclass outputStream = nullptr;

    jmethodID urlInit = nullptr;
    jmethodID openConnection = nullptr;
    jmethodID setRequestMethod = nullptr;
    jmethodID setRequestProperty = nullptr;
    jmethodID setDoOutput = nullptr;
    jmethodID setFixedLengthStreamingMode = nullptr;
    jmethodID setInstanceFollowRedirects = nullptr;
    jmethodID setUseCaches = nullptr;
    jmethodID setConnectTimeout = nullptr;
    jmethodID setReadTimeout = nullptr;
    jmethodID getOutputStream = nullptr;
    jmethodID getResponseCode = nullptr;
    jmethodID getHeaderFieldKey = nullptr;
    jmethodID getHeaderField = nullptr;
    jmethodID getContentLengthLong = nullptr;
    jmethodID getInputStream = nullptr;
    jmethodID getErrorStream = nullptr;
    jmethodID disconnect = nullptr;
    jmethodID read = nullptr;
    jmethodID closeInput = nullptr;
    jmethodID write = nullptr;
    jmethodID closeOutput = nullptr;
};

jint clampedMilliseconds(Time::MS timeout)
{
    auto maxMilliseconds = (std::int64_t) std::numeric_limits<jint>::max();
    return (jint) std::min(timeout.count, maxMilliseconds);
}

// The connection's own timeouts bound each connect and each read separately,
// so a slow drip of small reads can outlive them. At the deadline this
// disconnects from a thread of its own, which closes the socket under whatever
// call the request thread is blocked in, and the request reports a timeout.
class DisconnectAtDeadline
{
    using Clock = std::chrono::steady_clock;

public:
    DisconnectAtDeadline(JNIEnv* envToUse, jmethodID disconnectToUse)
        : env(envToUse)
        , disconnect(disconnectToUse)
    {
    }

    ~DisconnectAtDeadline()
    {
        stopWatchdog();

        if (connection != nullptr)
            env->DeleteGlobalRef(connection);
    }

    DisconnectAtDeadline(const DisconnectAtDeadline&) = delete;
    DisconnectAtDeadline& operator=(const DisconnectAtDeadline&) = delete;

    void start(jobject connectionToWatch, Time::MS timeout)
    {
        if (timeout.count <= 0)
            return;

        connection = env->NewGlobalRef(connectionToWatch);

        auto due = Clock::now() + std::chrono::milliseconds(timeout.count);
        auto watchUntilDue = [this, due] { watch(due); };
        watchdog = std::thread(watchUntilDue);
    }

    bool hasExpired() const { return expired.load(); }

private:
    void watch(Clock::time_point due)
    {
        auto lock = std::unique_lock(mutex);
        auto isFinished = [this] { return finished; };

        if (settled.wait_until(lock, due, isFinished))
            return;

        expired.store(true);
        lock.unlock();

        auto* watchEnv = Jni::currentEnv();

        if (watchEnv == nullptr)
            return;

        watchEnv->CallVoidMethod(connection, disconnect);
        watchEnv->ExceptionClear();
    }

    void stopWatchdog()
    {
        {
            auto lock = std::scoped_lock(mutex);
            finished = true;
        }

        settled.notify_all();

        if (watchdog.joinable())
            watchdog.join();
    }

    JNIEnv* env;
    jmethodID disconnect;
    jobject connection = nullptr;
    std::thread watchdog;
    std::mutex mutex;
    std::condition_variable settled;
    std::atomic<bool> expired {false};
    bool finished = false;
};

struct DisconnectOnExit
{
    ~DisconnectOnExit()
    {
        if (connection == nullptr)
            return;

        env->CallVoidMethod(connection, disconnect);
        env->ExceptionClear();
    }

    JNIEnv* env;
    jmethodID disconnect;
    const jobject& connection;
};

using ChunkSink = std::function<void(std::string_view)>;

// One request's connection. The members go in reverse: the watchdog is joined,
// then the connection disconnected, then the frame popped - which happens even
// when the constructor throws.
class Exchange
{
public:
    Exchange(JNIEnv* envToUse, const JavaNet& javaToUse, const Request& req)
        : env(envToUse)
        , java(javaToUse)
        , frame(envToUse)
        , disconnectOnExit {envToUse, javaToUse.disconnect, connection}
        , deadline(envToUse, javaToUse.disconnect)
    {
        open(req);
        deadline.start(connection, req.timeout);
        send(req);
        receiveStatusAndHeaders();
    }

    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;

    std::int64_t contentLength()
    {
        auto length = env->CallLongMethod(connection, java.getContentLengthLong);
        check();
        return length;
    }

    void readBody(const ChunkSink& consume)
    {
        if (isHead)
            return;

        auto* stream = env->CallObjectMethod(connection, bodyStreamMethod());

        if (stream == nullptr)
        {
            check();
            return;
        }

        auto closeStream = [this, stream]
        {
            env->CallVoidMethod(stream, java.closeInput);
            env->ExceptionClear();
        };

        try
        {
            readChunks(stream, consume);
        }
        catch (...)
        {
            closeStream();
            throw;
        }

        closeStream();
    }

    void throwIfExpired() const
    {
        if (deadline.hasExpired())
            throw std::runtime_error("The request timed out");
    }

    Response response;

private:
    void check()
    {
        auto message = Jni::takeException(env);
        throwIfExpired();

        if (!message.empty())
            throw std::runtime_error(message);
    }

    jobject newUrl(const std::string& text)
    {
        auto* javaText = Jni::toJava(env, std::string_view {text});
        check();

        auto* url = env->NewObject(java.url, java.urlInit, javaText);
        check();

        return url;
    }

    void open(const Request& req)
    {
        if (req.url.empty())
            throw std::invalid_argument("URL cannot be empty");

        auto* url = newUrl(req.url);
        connection = env->CallObjectMethod(url, java.openConnection);
        check();

        if (!env->IsInstanceOf(connection, java.connection))
            throw std::runtime_error("Not an HTTP URL: " + req.url);

        env->CallVoidMethod(connection, java.setInstanceFollowRedirects, JNI_TRUE);
        env->CallVoidMethod(connection, java.setUseCaches, JNI_FALSE);
        check();

        if (req.timeout.count > 0)
        {
            auto limit = clampedMilliseconds(req.timeout);
            env->CallVoidMethod(connection, java.setConnectTimeout, limit);
            env->CallVoidMethod(connection, java.setReadTimeout, limit);
            check();
        }

        auto* method = Jni::toJava(env, std::string_view {req.type});
        check();
        env->CallVoidMethod(connection, java.setRequestMethod, method);
        check();

        for (const auto& [key, value]: req.headers)
            setHeader(key, value);

        isHead = req.type == "HEAD";
    }

    void setHeader(const std::string& key, const std::string& value)
    {
        auto* javaKey = Jni::toJava(env, std::string_view {key});
        auto* javaValue = Jni::toJava(env, std::string_view {value});
        check();

        env->CallVoidMethod(connection, java.setRequestProperty, javaKey, javaValue);
        check();

        env->DeleteLocalRef(javaKey);
        env->DeleteLocalRef(javaValue);
    }

    void send(const Request& req)
    {
        if (req.body.empty())
            return;

        env->CallVoidMethod(connection, java.setDoOutput, JNI_TRUE);
        env->CallVoidMethod(
            connection, java.setFixedLengthStreamingMode, (jlong) req.body.size());
        check();

        auto* bytes = Jni::toJavaBytes(env, req.body);
        check();

        auto* stream = env->CallObjectMethod(connection, java.getOutputStream);
        check();

        env->CallVoidMethod(stream, java.write, bytes);
        auto message = Jni::takeException(env);

        env->CallVoidMethod(stream, java.closeOutput);

        if (message.empty())
            message = Jni::takeException(env);
        else
            env->ExceptionClear();

        throwIfExpired();

        if (!message.empty())
            throw std::runtime_error(message);
    }

    void receiveStatusAndHeaders()
    {
        response.statusCode = env->CallIntMethod(connection, java.getResponseCode);
        check();

        if (response.statusCode < 0)
            throw std::runtime_error("The response is not valid HTTP");

        for (auto index = jint {0};; ++index)
        {
            auto frameForHeader = Jni::LocalFrame {env, 4};
            auto* value =
                env->CallObjectMethod(connection, java.getHeaderField, index);
            check();

            if (value == nullptr)
                return;

            auto* key =
                env->CallObjectMethod(connection, java.getHeaderFieldKey, index);
            check();

            addHeader(Jni::toString(env, key), Jni::toString(env, value));
        }
    }

    // Index 0 is the status line, with no key; the X-Android-* fields are ones
    // the platform's client adds for itself, which no other backend reports.
    void addHeader(const std::string& key, const std::string& value)
    {
        if (key.empty() || key.starts_with("X-Android-"))
            return;

        addHeaderLine(key + ": " + value, response.headers);
    }

    jmethodID bodyStreamMethod() const
    {
        return response.statusCode >= 400 ? java.getErrorStream
                                          : java.getInputStream;
    }

    void readChunks(jobject stream, const ChunkSink& consume)
    {
        auto* buffer = env->NewByteArray(chunkSize);
        check();

        auto chunk = std::string((std::size_t) chunkSize, '\0');

        while (true)
        {
            throwIfExpired();

            auto count = env->CallIntMethod(stream, java.read, buffer);
            check();

            if (count < 0)
                return;

            env->GetByteArrayRegion(
                buffer, 0, count, reinterpret_cast<jbyte*>(chunk.data()));
            check();

            consume(std::string_view {chunk.data(), (std::size_t) count});
        }
    }

    JNIEnv* env;
    const JavaNet& java;
    Jni::LocalFrame frame;
    jobject connection = nullptr;
    DisconnectOnExit disconnectOnExit;
    DisconnectAtDeadline deadline;
    bool isHead = false;
};

struct JavaContext
{
    JNIEnv* env = nullptr;
    const JavaNet* net = nullptr;
};

JavaContext javaContext()
{
    auto* env = Jni::currentEnv();

    if (env == nullptr)
        throw std::runtime_error("No Java VM to make the request through");

    const auto* net = Jni::resolveOnce<JavaNet>(env);

    if (net == nullptr)
        throw std::runtime_error("java.net.HttpURLConnection is unavailable");

    return {env, net};
}

struct FileCloser
{
    void operator()(std::FILE* file) const { std::fclose(file); }
};

using File = std::unique_ptr<std::FILE, FileCloser>;

File openDestinationFile(const std::string& filePath)
{
    auto file = File {std::fopen(filePath.c_str(), "wb")};

    if (!file)
        throw std::runtime_error("Failed to open destination file");

    return file;
}

void throwIfCancelled(const Request& req)
{
    if (req.progress != nullptr && req.progress->cancel.load())
        throw std::runtime_error("Download cancelled");
}

Response httpRequestInternal(const Request& req)
{
    auto [env, net] = javaContext();
    auto exchange = Exchange {env, *net, req};

    auto& body = exchange.response.content;
    auto appendToBody = [&body](std::string_view chunk) { body.append(chunk); };
    exchange.readBody(appendToBody);

    return exchange.response;
}

Response downloadFileInternal(const Request& req, const std::string& filePath)
{
    auto [env, net] = javaContext();
    auto exchange = Exchange {env, *net, req};

    if (req.progress != nullptr)
        req.progress->totalBytes.store(exchange.contentLength());

    throwIfCancelled(req);

    auto file = openDestinationFile(filePath);
    auto received = std::int64_t {0};

    auto writeToFile = [&](std::string_view chunk)
    {
        if (std::fwrite(chunk.data(), 1, chunk.size(), file.get()) != chunk.size())
            throw std::runtime_error("Failed to write file: " + filePath);

        received += (std::int64_t) chunk.size();

        if (req.progress != nullptr)
            req.progress->bytesReceived.store(received);

        throwIfCancelled(req);
    };

    exchange.readBody(writeToFile);

    if (std::fflush(file.get()) != 0)
        throw std::runtime_error("Failed to write file: " + filePath);

    return exchange.response;
}

} // namespace

Response httpRequest(const Request& req)
{
    auto res = Response();

    try
    {
        return httpRequestInternal(req);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }

    return res;
}

Response downloadFile(const Request& req, const std::string& filePath)
{
    auto res = Response();

    try
    {
        res = downloadFileInternal(req, filePath);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }

    if (req.progress != nullptr)
        req.progress->done.store(true);

    return res;
}

} // namespace eacp::HTTP
