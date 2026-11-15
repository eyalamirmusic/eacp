#include <eacp/Core/Core.h>
#include <eacp/Network/HTTP/HttpProtocol.h>
#include <eacp/Network/Network.h>

using namespace eacp;

// The web's network client against Tests/Network/Web/TestServer.mjs (its URL
// the last argument), used as an app uses it: nothing blocks. In a browser
// (HelloNetwork-run, or the HelloNetworkHeadless ctest entry) it exercises
// the browser's own rules - CORS, forbidden headers, opaque redirects - which
// the node-run suites cannot.
namespace
{
using Done = std::function<void(bool passed, const std::string& detail)>;

struct Check
{
    std::string name;
    std::function<void(Done)> run = [](Done) {};
};

constexpr auto answerLimit = Time::MS {10000};
constexpr auto downloadSize = 300000;

std::string describe(const HTTP::Response& response)
{
    if (!response.error.empty())
        return response.error;

    return "status " + std::to_string(response.statusCode);
}

std::string pattern(int size)
{
    auto bytes = std::string((std::size_t) size, '\0');

    for (auto i = 0; i < size; ++i)
        bytes[(std::size_t) i] = (char) (i % 251);

    return bytes;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

std::string serverFromArguments()
{
    auto server = std::string {};

    for (const auto& arg: Apps::commandLineArgs())
        if (arg.starts_with("http://") || arg.starts_with("https://"))
            server = arg;

    while (server.ends_with("/"))
        server.pop_back();

    return server;
}

void request(
    const HTTP::Request& req,
    std::function<std::pair<bool, std::string>(const HTTP::Response&)> judge,
    const Done& done)
{
    HTTP::asyncRequest(req,
                       [judge, done](const HTTP::Response& response)
                       {
                           auto [passed, detail] = judge(response);
                           done(passed, detail);
                       });
}
} // namespace

struct HelloNetwork
{
    HelloNetwork()
    {
        if (server.empty())
        {
            Console::printError("usage: HelloNetwork <test server URL>, e.g. "
                                "node Tests/Network/Web/TestServer.mjs serve");
            Apps::quit(1);
            return;
        }

        Console::print(Strings::concat("HelloNetwork on ",
                                       Platform::name(),
                                       " against ",
                                       server,
                                       ", ",
                                       (int) checks.size(),
                                       " checks"));
        runNext();
    }

    ~HelloNetwork()
    {
        *alive = false;
        HTTP::cancelAllAsyncRequests();
    }

    std::string url(const std::string& path) const { return server + path; }

    std::string webSocketUrl(const std::string& path) const
    {
        return "ws" + server.substr(4) + path;
    }

    void runNext()
    {
        if (next == (int) checks.size())
            return finish();

        auto index = next++;
        auto reported = std::make_shared<bool>(false);
        auto report = [this, alive = alive, index, reported](
                          bool passed, const std::string& detail)
        {
            if (!*alive || std::exchange(*reported, true))
                return;

            record(index, passed, detail);
            Threads::callAsync(
                [this, alive]
                {
                    if (*alive)
                        runNext();
                });
        };

        Threads::callAfter(answerLimit,
                           [report] { report(false, "no answer in time"); });
        checks[(std::size_t) index].run(report);
    }

    void record(int index, bool passed, const std::string& detail)
    {
        failures += passed ? 0 : 1;
        Console::print(Strings::concat(passed ? "  ok    " : "  FAIL  ",
                                       checks[(std::size_t) index].name,
                                       detail.empty() ? "" : ": ",
                                       detail));
    }

    void finish()
    {
        webSocket.reset();
        Console::print(failures == 0 ? "PASS" : "FAIL");
        Apps::quit(failures == 0 ? 0 : 1);
    }

    void checkBlockingIsRefused(const Done& done)
    {
        auto response = HTTP::httpRequest(HTTP::Request {url("/get")});
        auto refused = response.statusCode == 0
                       && contains(response.error, "eacp-core-nested-pump");

        auto fetchRefused = false;

        try
        {
            auto options = OnlineResource::Options {};
            options.info.url = url("/get");
            options.directory = FilePath {"/tmp/eacp-hello-network-blocking"};
            OnlineResource::fetch(options);
        }
        catch (const std::exception& e)
        {
            fetchRefused = contains(e.what(), "eacp-core-nested-pump");
        }

        done(refused && fetchRefused, response.error);
    }

    void checkGet(const Done& done)
    {
        request(
            HTTP::Request {url("/get")},
            [](const HTTP::Response& response)
            {
                auto custom =
                    HTTP::findHeaderIgnoringCase(response.headers, "X-Custom");
                auto passed = response.statusCode == 200
                              && response.content.starts_with("hello from ")
                              && custom == "custom-value";
                return std::pair {passed,
                                  describe(response) + ", X-Custom "
                                      + (custom.empty() ? "hidden" : custom)};
            },
            done);
    }

    void checkPost(const Done& done)
    {
        auto req = HTTP::Request::post(url("/echo"), "posted-body");
        req.headers["X-Test"] = "abc";
        req.headers["Host"] = "elsewhere.example";
        HTTP::setDefaultUserAgent("HelloNetwork/1.0");

        request(
            req,
            [](const HTTP::Response& response)
            {
                HTTP::setDefaultUserAgent("");
                auto body = response.content;
                auto passed = response.statusCode == 200
                              && contains(body, "\"method\":\"POST\"")
                              && contains(body, "posted-body")
                              && contains(body, "\"x-test\":\"abc\"")
                              && !contains(body, "elsewhere.example");
                auto agent = contains(body, "HelloNetwork/1.0")
                                 ? "our User-Agent sent"
                                 : "our User-Agent replaced by the runtime's";
                return std::pair {passed, describe(response) + ", " + agent};
            },
            done);
    }

    void checkNotFound(const Done& done)
    {
        request(
            HTTP::Request {url("/status?code=404")},
            [](const HTTP::Response& response)
            {
                return std::pair {response.statusCode == 404
                                      && response.error.empty(),
                                  describe(response)};
            },
            done);
    }

    void checkTimeout(const Done& done)
    {
        auto req = HTTP::Request {url("/slow-headers?ms=3000")};
        req.timeout = Time::MS {300};

        request(
            req,
            [](const HTTP::Response& response)
            {
                return std::pair {response.error == HTTP::timedOutError(),
                                  describe(response)};
            },
            done);
    }

    void checkFollowedRedirects(const Done& done)
    {
        request(
            HTTP::Request {url("/chain?n=3")},
            [](const HTTP::Response& response)
            {
                return std::pair {
                    response.statusCode == 200
                        && contains(response.content, "\"path\":\"/echo\""),
                    describe(response)};
            },
            done);
    }

    // A browser hides a redirect it is told not to follow; node returns it.
    void checkUnfollowedRedirect(const Done& done)
    {
        auto req = HTTP::Request {url("/redirect?code=302&to=/get")};
        req.followRedirects = false;

        request(
            req,
            [](const HTTP::Response& response)
            {
                auto hidden = contains(response.error, "exposes neither");
                auto returned = response.statusCode == 302;
                auto detail = hidden     ? "hidden by the runtime, as a browser does"
                              : returned ? "returned whole, as node does"
                                         : describe(response);
                return std::pair {hidden || returned, std::string {detail}};
            },
            done);
    }

    void checkDownload(const Done& done)
    {
        auto download = HTTP::DownloadRequest {
            url("/bytes?size=" + std::to_string(downloadSize)),
            "/tmp/eacp-hello-network-download.bin"};

        HTTP::asyncDownload(
            download,
            [path = download.filePath, done](const HTTP::Response& r)
            {
                auto readBack = Files::readFile(path);
                done(r.statusCode == 200 && readBack == pattern(downloadSize),
                     describe(r) + ", " + std::to_string(readBack.size())
                         + " bytes read back");
            });
    }

    // Twice: the second revalidates with If-None-Match, which a browser
    // preflights, and keeps the copy on the server's 304.
    void checkOnlineResource(const Done& done)
    {
        auto options = OnlineResource::Options {};
        options.info.url = url("/etag?tag=hello-network&size=5000");
        options.info.fileName = "etag.bin";
        options.directory = FilePath {"/tmp/eacp-hello-network"};

        auto second = [options, done](const OnlineResource::Result& first)
        {
            if (!first.ok || !first.downloaded)
                return done(false, "first fetch: " + first.error);

            auto again = options;
            again.onFinished = [done](const OnlineResource::Result& result)
            {
                done(result.ok && !result.downloaded,
                     result.ok
                         ? (result.downloaded ? "downloaded again" : "kept on 304")
                         : result.error);
            };
            OnlineResource::fetchAsync(again);
        };

        auto first = options;
        first.onFinished = second;
        OnlineResource::fetchAsync(first);
    }

    void checkWebSocket(const Done& done)
    {
        auto callbacks = WebSocket::Callbacks {};
        callbacks.onOpen = [this](const std::string&) { webSocket->send("hello"); };
        callbacks.onMessage = [this](const WebSocket::Message& message)
        {
            echoed = message.data;
            webSocket->close(1000, "bye");
        };
        callbacks.onError = [done](const std::string& error) { done(false, error); };
        callbacks.onClose = [this, done](const WebSocket::CloseStatus& status)
        {
            done(echoed == "hello" && status.code == 1000,
                 "echoed \"" + echoed + "\", closed with "
                     + std::to_string(status.code));
        };

        webSocket =
            std::make_unique<WebSocket::Connection>(webSocketUrl("/ws"), callbacks);
    }

    std::string server = serverFromArguments();
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::unique_ptr<WebSocket::Connection> webSocket;
    std::string echoed;
    int next = 0;
    int failures = 0;

    std::vector<Check> checks {
        {"a blocking call is refused",
         [this](Done d) { checkBlockingIsRefused(d); }},
        {"GET", [this](Done d) { checkGet(d); }},
        {"POST", [this](Done d) { checkPost(d); }},
        {"404", [this](Done d) { checkNotFound(d); }},
        {"timeout", [this](Done d) { checkTimeout(d); }},
        {"followed redirects", [this](Done d) { checkFollowedRedirects(d); }},
        {"unfollowed redirect", [this](Done d) { checkUnfollowedRedirect(d); }},
        {"download and read back", [this](Done d) { checkDownload(d); }},
        {"fetchAsync twice", [this](Done d) { checkOnlineResource(d); }},
        {"WebSocket echo and close", [this](Done d) { checkWebSocket(d); }},
    };
};

int main(int argc, char* argv[])
{
    return Apps::run<HelloNetwork>(argc, argv);
}
