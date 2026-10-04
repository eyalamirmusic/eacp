#include <eacp/GPU/GPU.h>
#include <eacp/Network/Network.h>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace eacp;
using namespace GPU;
using namespace Maths;

struct Vertex
{
    Vec2 position;
    Graphics::Color color;
};

EACP_SHADER_VALUE(Graphics::Color, Float4)

namespace
{
const auto exampleUrl = std::string {"https://example.com/"};
const auto smallFileUrl = std::string {"https://www.gstatic.com/webp/gallery/5.jpg"};
const auto largeFileUrl =
    std::string {"https://www.python.org/ftp/python/3.12.0/Python-3.12.0.tar.xz"};
const auto resourceUrl = std::string {"https://www.google.com/robots.txt"};
const auto unroutableUrl = std::string {"http://10.255.255.1/"};
const auto refusedWebSocketUrl = std::string {"ws://127.0.0.1:1/"};
const auto tlsEchoUrl = std::string {"wss://echo.websocket.org/"};
const auto tlsGreetingPrefix = std::string {"Request served by"};
constexpr auto largeMessageSize = std::size_t {200 * 1024};

constexpr auto requestTimeout = Time::MS {30000};
constexpr auto stepTimeout = Time::MS {60000};
constexpr auto cancelAfterBytes = std::int64_t {256 * 1024};
constexpr auto unroutableTimeout = Time::MS {1000};
constexpr auto unroutableLimit = std::chrono::milliseconds {3000};

using Clock = std::chrono::steady_clock;

enum class Outcome
{
    running,
    passed,
    failed
};

struct Result
{
    bool passed = false;
    bool skipped = false;
    std::string detail;
};

Result pass(const std::string& detail)
{
    return {true, false, detail};
}

Result fail(const std::string& detail)
{
    return {false, false, detail};
}

Result skip(const std::string& detail)
{
    return {true, true, detail};
}

using Done = std::function<void(const Result&)>;

void report(const std::string& line)
{
    LOG("HelloNetwork: ", line);
}

bool contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

std::string lowercase(std::string text)
{
    for (auto& c: text)
        c = (char) std::tolower((unsigned char) c);

    return text;
}

std::string headerValue(const HTTP::Response& response, const std::string& name)
{
    for (const auto& [key, value]: response.headers)
        if (lowercase(key) == lowercase(name))
            return value;

    return {};
}

std::string describe(const HTTP::Response& response)
{
    auto text = "status " + std::to_string(response.statusCode) + ", "
                + std::to_string(response.content.size()) + " bytes";

    if (!response.error.empty())
        text += ", error \"" + response.error + "\"";

    return text;
}

std::int64_t fileSize(const std::string& path)
{
    auto error = std::error_code {};
    auto size = std::filesystem::file_size(path, error);
    return error ? -1 : (std::int64_t) size;
}

std::string cachePath(const std::string& name)
{
    auto folder = FilePath::appCacheDirectory();
    auto error = std::error_code {};
    std::filesystem::create_directories(folder.str(), error);
    return (folder / name).str();
}

std::string binaryPayload(std::size_t size)
{
    auto bytes = std::string(size, '\0');

    for (auto index = std::size_t {0}; index < size; ++index)
        bytes[index] = (char) ((index * 31 + index / 251) & 0xff);

    return bytes;
}

std::string textPayload(std::size_t size)
{
    const auto alphabet = std::string_view {"abcdefghijklmnopqrstuvwxyz0123456789"};
    auto text = std::string(size, ' ');

    for (auto index = std::size_t {0}; index < size; ++index)
        text[index] = alphabet[(index * 7 + index / 97) % alphabet.size()];

    return text;
}

std::string describe(const WebSocket::Message& message)
{
    auto type = message.type == WebSocket::MessageType::binary ? "binary" : "text";
    return std::string {type} + " of " + std::to_string(message.data.size())
           + " bytes";
}

bool sameMessage(const WebSocket::Message& a, const WebSocket::Message& b)
{
    return a.type == b.type && a.data == b.data;
}

struct EchoExchange
{
    Vector<WebSocket::Message> expected;
    int received = 0;
    std::string mismatch;
};

struct TlsExchange
{
    std::string sent = "hello from eacp on android";
    bool greeted = false;
    bool echoed = false;
    std::string unexpected;
};

using Work = std::function<HTTP::Response()>;

void performOnWorker(const Work& work, const HTTP::ResponseCallback& callback)
{
    auto run = [work, callback]
    {
        auto response = work();
        auto deliver = [callback, response] { callback(response); };
        Threads::callAsync(deliver);
    };

    std::thread(run).detach();
}

const Array<Vertex, 3> triangleVertices = {
    Vertex {{0.0f, 0.5f}, {1.0f, 1.0f, 1.0f}},
    Vertex {{-0.5f, -0.3f}, {0.2f, 0.2f, 0.2f}},
    Vertex {{0.5f, -0.3f}, {0.6f, 0.6f, 0.6f}},
};

struct TriangleShader final : ShaderProgram
{
    TriangleShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto color = vertexInput(&Vertex::color);

        auto c = cos(angle);
        auto s = sin(angle);
        auto px = position.x();
        auto py = position.y();
        auto rotated = float2(px * c - py * s, (px * s + py * c) * aspect);

        setPosition(float4(rotated, 0.0f, 1.0f));
        setFragment(varying(color));
    }

    Uniform<Float> angle;
    Uniform<Float> aspect;

    EACP_SHADER(angle, aspect)
};

Graphics::Color colorFor(Outcome outcome)
{
    switch (outcome)
    {
        case Outcome::running:
            return {0.95f, 0.65f, 0.1f};
        case Outcome::passed:
            return {0.15f, 0.7f, 0.3f};
        case Outcome::failed:
            return {0.85f, 0.15f, 0.15f};
    }

    return {};
}
} // namespace

struct StatusView final : GPUView
{
    StatusView()
    {
        setContinuous(true);
        triangle.setVertices(triangleVertices);
        triangle.prepare(sampleCount());
    }

    void update(Threads::FrameTime time) override
    {
        angle += 1.2f * static_cast<float>(time.delta);
    }

    void render(Frame& frame) override
    {
        auto size = frame.logicalSize();
        auto pass = frame.beginPass({colorFor(outcome)});

        triangle.angle = angle;
        triangle.aspect = size.x / std::max(size.y, 1.f);
        pass.draw(triangle);
    }

    TriangleShader triangle;
    Outcome outcome = Outcome::running;
    float angle = 0.f;
};

class NetworkChecks
{
public:
    using OutcomeCallback = std::function<void(Outcome)>;

    ~NetworkChecks() { HTTP::cancelAllAsyncRequests(); }

    void start(const OutcomeCallback& onOutcomeToUse)
    {
        onOutcome = onOutcomeToUse;
        steps = {
            {"https-get", [this](const Done& done) { httpsGet(done); }},
            {"loopback", [this](const Done& done) { loopbackGet(done); }},
            {"download", [this](const Done& done) { download(done); }},
            {"timeout", [this](const Done& done) { unroutable(done); }},
            {"online-resource", [this](const Done& done) { onlineResource(done); }},
            {"websocket", [this](const Done& done) { webSocketEcho(done); }},
            {"websocket-refused",
             [this](const Done& done) { webSocketRefused(done); }},
            {"websocket-tls", [this](const Done& done) { webSocketTls(done); }},
        };

        runNext();
    }

private:
    struct Step
    {
        std::string name;
        std::function<void(const Done&)> run;
    };

    template <typename Function>
    auto whileAlive(const Function& function) const
    {
        auto token = std::weak_ptr<bool> {alive};

        return [token, function](const auto&... args)
        {
            if (token.lock())
                function(args...);
        };
    }

    void runNext()
    {
        if (current == (int) steps.size())
        {
            finishAll();
            return;
        }

        auto index = current;
        auto finish = [this, index](const Result& result)
        { finishStep(index, result); };
        auto expire = [this, index]
        { finishStep(index, fail("no answer within 60 s")); };

        Threads::callAfter(stepTimeout, whileAlive(expire));
        report("RUN " + steps[index].name);
        steps[index].run(whileAlive(finish));
    }

    void finishStep(int index, const Result& result)
    {
        if (index != current)
            return;

        const auto& name = steps[index].name;
        auto verdict = result.skipped ? "SKIP " : result.passed ? "PASS " : "FAIL ";
        report(verdict + name + ": " + result.detail);

        if (!result.passed)
            failures.add(name);

        ++current;
        runNext();
    }

    void finishAll()
    {
        if (failures.empty())
        {
            report("ALL PASSED");
            onOutcome(Outcome::passed);
            return;
        }

        auto names = std::string {};

        for (const auto& name: failures)
            names += (names.empty() ? "" : " ") + name;

        report("FAILED " + names);
        onOutcome(Outcome::failed);
    }

    void httpsGet(const Done& done)
    {
        auto request = HTTP::Request {exampleUrl};
        request.timeout = requestTimeout;

        auto onResponse = [done](const HTTP::Response& response)
        {
            if (response.statusCode != 200)
                return done(fail("expected 200, got " + describe(response)));

            if (!contains(response.content, "Example Domain"))
                return done(
                    fail("body lacks \"Example Domain\": " + describe(response)));

            auto type = headerValue(response, "Content-Type");

            if (type.empty())
                return done(fail("no Content-Type header"));

            done(pass(describe(response) + ", Content-Type " + type));
        };

        HTTP::asyncRequest(request, whileAlive(onResponse));
    }

    std::string loopbackUrl(const std::string& path) const
    {
        return "http://127.0.0.1:" + std::to_string(httpServer->boundPort()) + path;
    }

    void loopbackGet(const Done& done)
    {
        httpServer = std::make_unique<HTTP::Server>();

        auto hello = [](const HTTP::Request&)
        {
            auto response = HTTP::Response {};
            response.statusCode = 200;
            response.setContent("hello from loopback", "text/plain");
            return response;
        };

        auto echo = [](const HTTP::Request& request)
        {
            auto response = HTTP::Response {};
            response.statusCode = 200;
            response.setContent(request.body, "application/json");
            return response;
        };

        httpServer->get("/hello", hello);
        httpServer->post("/echo", echo);

        if (!httpServer->listen(0))
            return done(fail("HTTP::Server could not listen on 127.0.0.1"));

        auto request = HTTP::Request {loopbackUrl("/hello")};
        request.timeout = requestTimeout;

        auto onResponse = [this, done](const HTTP::Response& response)
        {
            if (response.statusCode != 200
                || response.content != "hello from loopback")
                return done(fail("GET /hello: " + describe(response)));

            loopbackPost(done);
        };

        HTTP::asyncRequest(request, whileAlive(onResponse));
    }

    void loopbackPost(const Done& done)
    {
        auto body = std::string {R"({"greeting":"hello","count":3})"};
        auto request = HTTP::Request::post(loopbackUrl("/echo"), body);
        request.headers["Content-Type"] = "application/json";
        request.timeout = requestTimeout;

        auto onResponse = [this, done, body](const HTTP::Response& response)
        {
            if (response.statusCode != 200 || response.content != body)
                return done(fail("POST /echo: " + describe(response) + ", body \""
                                 + response.content + "\""));

            loopbackMissing(done);
        };

        HTTP::asyncRequest(request, whileAlive(onResponse));
    }

    void loopbackMissing(const Done& done)
    {
        auto request = HTTP::Request {loopbackUrl("/missing")};
        request.timeout = requestTimeout;

        auto onResponse = [this, done](const HTTP::Response& response)
        {
            if (response.statusCode != 404)
                return done(fail("GET /missing: " + describe(response)));

            auto port = std::to_string(httpServer->boundPort());
            httpServer.reset();
            done(pass("GET 200, POST echoed, 404 on port " + port));
        };

        HTTP::asyncRequest(request, whileAlive(onResponse));
    }

    void download(const Done& done)
    {
        auto path = cachePath("gallery-5.jpg");
        auto progress = std::make_shared<HTTP::DownloadProgress>();

        auto work = [path, progress]
        {
            auto request = HTTP::Request {smallFileUrl};
            request.timeout = requestTimeout;
            request.progress = progress.get();
            return request.downloadTo(path);
        };

        auto onResponse =
            [this, done, path, progress](const HTTP::Response& response)
        {
            if (response.statusCode != 200 || !response.error.empty())
                return done(fail("download: " + describe(response)));

            auto size = fileSize(path);
            auto declared = headerValue(response, "Content-Length");
            auto total = progress->totalBytes.load();
            auto received = progress->bytesReceived.load();
            auto detail = "file " + std::to_string(size) + " bytes, Content-Length "
                          + declared + ", totalBytes " + std::to_string(total)
                          + ", bytesReceived " + std::to_string(received);

            if (size <= 0 || declared != std::to_string(size) || total != size
                || received != size || !progress->done.load())
                return done(fail(detail));

            report("  download: " + detail);
            cancelledDownload(done);
        };

        performOnWorker(work, whileAlive(onResponse));
    }

    void cancelOnceUnderWay(const std::shared_ptr<HTTP::DownloadProgress>& progress)
    {
        if (progress->done.load())
            return;

        if (progress->bytesReceived.load() >= cancelAfterBytes)
        {
            progress->cancel.store(true);
            return;
        }

        auto again = [this, progress] { cancelOnceUnderWay(progress); };
        Threads::callAfter(Time::MS {10}, whileAlive(again));
    }

    void cancelledDownload(const Done& done)
    {
        auto path = cachePath("cancelled.tar.xz");
        auto progress = std::make_shared<HTTP::DownloadProgress>();

        auto work = [path, progress]
        {
            auto request = HTTP::Request {largeFileUrl};
            request.timeout = requestTimeout;
            request.progress = progress.get();
            return request.downloadTo(path);
        };

        auto onResponse = [done, path, progress](const HTTP::Response& response)
        {
            auto received = progress->bytesReceived.load();
            auto total = progress->totalBytes.load();
            auto detail = "cancelled at " + std::to_string(received) + " of "
                          + std::to_string(total) + " bytes, " + describe(response);
            std::filesystem::remove(path);

            if (!progress->cancel.load()
                || !contains(lowercase(response.error), "cancel")
                || received >= total || !progress->done.load())
                return done(fail(detail));

            done(pass("complete download and a " + detail));
        };

        performOnWorker(work, whileAlive(onResponse));
        cancelOnceUnderWay(progress);
    }

    void unroutable(const Done& done)
    {
        auto request = HTTP::Request {unroutableUrl};
        request.timeout = unroutableTimeout;
        auto started = Clock::now();

        auto onResponse = [done, started](const HTTP::Response& response)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - started);
            auto detail = describe(response) + " after "
                          + std::to_string(elapsed.count()) + " ms";

            if (response.error.empty() || elapsed > unroutableLimit)
                return done(fail(detail));

            done(pass(detail));
        };

        HTTP::asyncRequest(request, whileAlive(onResponse));
    }

    OnlineResource::Options resourceOptions() const
    {
        auto options = OnlineResource::Options {};
        options.info.url = resourceUrl;
        options.info.timeout = requestTimeout;
        options.directory = FilePath::appCacheDirectory() / "Resources";
        return options;
    }

    void onlineResource(const Done& done)
    {
        auto error = std::error_code {};
        std::filesystem::remove_all(resourceOptions().directory.str(), error);

        auto onFirst = [this, done](const OnlineResource::Result& first)
        {
            if (!first.ok || !first.downloaded)
                return done(fail("first fetch: ok " + std::to_string(first.ok)
                                 + ", downloaded " + std::to_string(first.downloaded)
                                 + ", error \"" + first.error + "\""));

            onlineResourceAgain(done, first.path.str());
        };

        auto onError = [done](const std::string& message)
        { done(fail("first fetch rejected: " + message)); };

        OnlineResource::fetchAsync(resourceOptions())
            .then(whileAlive(onFirst), whileAlive(onError));
    }

    void onlineResourceAgain(const Done& done, const std::string& path)
    {
        auto onSecond = [done, path](const OnlineResource::Result& second)
        {
            auto detail = path + ", " + std::to_string(fileSize(path)) + " bytes";

            if (!second.ok || second.downloaded)
                return done(fail("second fetch: ok " + std::to_string(second.ok)
                                 + ", downloaded "
                                 + std::to_string(second.downloaded) + ", error \""
                                 + second.error + "\""));

            done(pass("downloaded once, then a cache hit: " + detail));
        };

        auto onError = [done](const std::string& message)
        { done(fail("second fetch rejected: " + message)); };

        OnlineResource::fetchAsync(resourceOptions())
            .then(whileAlive(onSecond), whileAlive(onError));
    }

    void webSocketEcho(const Done& done)
    {
        if (!WebSocket::Connection::isSupported())
            return done(skip("WebSocket::Connection::isSupported() is false"));

        auto serverCallbacks = WebSocket::ServerCallbacks {};
        serverCallbacks.onMessage =
            [this](WebSocket::ClientId client, const WebSocket::Message& message)
        {
            if (message.type == WebSocket::MessageType::binary)
                webSocketServer->sendBinary(client, message.data);
            else
                webSocketServer->send(client, message.data);
        };

        webSocketServer = std::make_unique<WebSocket::Server>(serverCallbacks);

        if (!webSocketServer->listen(0))
            return done(fail("WebSocket::Server could not listen"));

        auto exchange = std::make_shared<EchoExchange>();
        exchange->expected = {
            {"hello over websocket", WebSocket::MessageType::text},
            {binaryPayload(largeMessageSize), WebSocket::MessageType::binary},
            {textPayload(largeMessageSize), WebSocket::MessageType::text},
        };

        auto callbacks = WebSocket::Callbacks {};
        callbacks.onOpen = [this, exchange](const std::string&)
        {
            for (const auto& message: exchange->expected)
            {
                if (message.type == WebSocket::MessageType::binary)
                    webSocketClient->sendBinary(message.data);
                else
                    webSocketClient->send(message.data);
            }
        };
        callbacks.onMessage = [this, exchange](const WebSocket::Message& message)
        {
            auto index = exchange->received++;

            if (index >= (int) exchange->expected.size())
                exchange->mismatch = "unexpected extra " + describe(message);
            else if (!sameMessage(message, exchange->expected[index]))
                exchange->mismatch = "echo " + std::to_string(index)
                                     + " differs: got " + describe(message)
                                     + ", sent "
                                     + describe(exchange->expected[index]);

            if (!exchange->mismatch.empty()
                || exchange->received == (int) exchange->expected.size())
                webSocketClient->close();
        };
        callbacks.onError = [done](const std::string& error)
        { done(fail("client error: " + error)); };
        callbacks.onClose = [done, exchange](const WebSocket::CloseStatus& status)
        {
            auto detail = std::to_string(exchange->received) + " of "
                          + std::to_string(exchange->expected.size())
                          + " echoes byte-exact (short text, 200 KB binary, 200 KB "
                            "text), closed "
                          + std::to_string(status.code);

            if (!exchange->mismatch.empty())
                return done(fail(exchange->mismatch + "; " + detail));

            if (exchange->received != (int) exchange->expected.size()
                || status.code != 1000)
                return done(fail(detail));

            done(pass(detail));
        };

        auto url =
            "ws://127.0.0.1:" + std::to_string(webSocketServer->boundPort()) + "/";
        webSocketClient = std::make_unique<WebSocket::Connection>(url, callbacks);
    }

    void webSocketRefused(const Done& done)
    {
        if (!WebSocket::Connection::isSupported())
            return done(skip("WebSocket::Connection::isSupported() is false"));

        auto started = Clock::now();
        auto elapsed = [started]
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - started);
            return std::to_string(ms.count()) + " ms";
        };

        auto callbacks = WebSocket::Callbacks {};
        callbacks.onOpen = [done](const std::string&)
        { done(fail("opened a connection to a port nobody listens on")); };
        callbacks.onError = [done, elapsed](const std::string& error)
        { done(pass("error \"" + error + "\" after " + elapsed())); };
        callbacks.onClose = [done, elapsed](const WebSocket::CloseStatus& status)
        {
            done(fail("closed " + std::to_string(status.code)
                      + " with no onError after " + elapsed()));
        };

        refusedClient =
            std::make_unique<WebSocket::Connection>(refusedWebSocketUrl, callbacks);
    }

    void webSocketTls(const Done& done)
    {
        if (!WebSocket::Connection::isSupported())
            return done(skip("WebSocket::Connection::isSupported() is false"));

        auto exchange = std::make_shared<TlsExchange>();

        auto callbacks = WebSocket::Callbacks {};
        callbacks.onOpen = [this, exchange](const std::string&)
        { tlsClient->send(exchange->sent); };
        callbacks.onMessage = [this, exchange](const WebSocket::Message& message)
        {
            if (sameMessage(message, {exchange->sent, WebSocket::MessageType::text}))
            {
                exchange->echoed = true;
                tlsClient->close();
                return;
            }

            if (!exchange->greeted && message.data.starts_with(tlsGreetingPrefix))
            {
                exchange->greeted = true;
                report("  websocket-tls greeting: " + message.data);
                return;
            }

            exchange->unexpected = describe(message) + ": \"" + message.data + "\"";
            tlsClient->close();
        };
        callbacks.onError = [done](const std::string& error)
        { done(fail("client error: " + error)); };
        callbacks.onClose = [done, exchange](const WebSocket::CloseStatus& status)
        {
            auto detail = std::string {exchange->greeted ? "greeting skipped, " : ""}
                          + "echo " + (exchange->echoed ? "received" : "missing")
                          + ", closed " + std::to_string(status.code) + " \""
                          + status.reason + "\"";

            if (!exchange->unexpected.empty())
                return done(
                    fail("unexpected " + exchange->unexpected + "; " + detail));

            if (!exchange->echoed || status.code != 1000)
                return done(fail(detail));

            done(pass(detail));
        };

        tlsClient = std::make_unique<WebSocket::Connection>(tlsEchoUrl, callbacks);
    }

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    OutcomeCallback onOutcome = [](Outcome) {};
    Vector<Step> steps;
    Vector<std::string> failures;
    int current = 0;

    std::unique_ptr<HTTP::Server> httpServer;
    std::unique_ptr<WebSocket::Server> webSocketServer;
    std::unique_ptr<WebSocket::Connection> webSocketClient;
    std::unique_ptr<WebSocket::Connection> refusedClient;
    std::unique_ptr<WebSocket::Connection> tlsClient;
};

struct HelloNetwork
{
    HelloNetwork()
    {
        auto showOutcome = [this](Outcome outcome) { shown.view.outcome = outcome; };
        checks.start(showOutcome);
    }

    Graphics::ViewWindow<StatusView> shown;
    NetworkChecks checks;
};

int main()
{
    report(Device::shared().isValid() ? "Vulkan device up" : "no Vulkan device");
    return Apps::run<HelloNetwork>();
}
