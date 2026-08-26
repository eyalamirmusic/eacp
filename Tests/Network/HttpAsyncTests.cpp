#include "Common.h"
#include <filesystem>
#include <fstream>
#include <system_error>

using namespace nano;
using eacp::HTTP::asyncDownload;
using eacp::HTTP::asyncRequest;
using eacp::HTTP::cancelAllAsyncRequests;
using eacp::HTTP::DownloadRequest;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::HTTP::ServerOptions;
using eacp::HTTP::ServerThreadingMode;
using eacp::Threads::runEventLoopUntil;
using eacp::Time::MS;

namespace
{
constexpr auto pumpTimeout = MS {5000};

std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

std::string tempPath(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path() / ("eacp-async-" + name);
    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
    return path.string();
}

std::string readFile(const std::string& path)
{
    auto file = std::ifstream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

Response respondWith(const std::string& content)
{
    auto res = Response();
    res.statusCode = 200;
    res.content = content;
    return res;
}

Response stallThenRespond(StallGate& gate, const std::string& content)
{
    gate.wait();
    return respondWith(content);
}

// A handler that blocks has to run off the event loop, which is the same
// loop the reply has to travel back through.
ServerOptions threadPoolOptions()
{
    auto options = ServerOptions();
    options.threading = ServerThreadingMode::ThreadPool;
    options.threadPoolSize = 2;
    return options;
}

bool pumpUntil(const std::function<bool()>& ready)
{
    return runEventLoopUntil(ready, pumpTimeout);
}

void pumpFor(MS duration)
{
    auto never = [] { return false; };
    runEventLoopUntil(never, duration);
}
} // namespace

auto tDownloadRequestFields = test("HttpAsync/downloadRequestCarriesUrlAndPath") = []
{
    auto req = DownloadRequest("http://example.com/f.bin", "/tmp/f.bin");
    check(req.url == "http://example.com/f.bin");
    check(req.filePath == "/tmp/f.bin");
    check(req.type == "GET");
    check(req.timeout.count == 0);

    auto empty = DownloadRequest();
    check(empty.url.empty());
    check(empty.filePath.empty());
};

auto tDelivers = test("HttpAsync/deliversOnTheMessageThreadExactlyOnce") = []
{
    auto server = Server();

    auto handler = [](const Request&) { return respondWith("hello"); };
    check(server.listen(0, handler));

    auto received = Response();
    auto calls = 0;
    auto onMessageThread = false;

    auto onResponse = [&](const Response& res)
    {
        received = res;
        onMessageThread = eacp::Threads::isMainThread();
        ++calls;
    };

    asyncRequest(Request(baseUrl(server.boundPort()) + "/ping"), onResponse);

    auto arrived = [&] { return calls > 0; };
    check(pumpUntil(arrived));
    pumpFor(MS {200});

    check(calls == 1);
    check(onMessageThread);
    check(received.statusCode == 200);
    check(received.content == "hello");
    check(received.error.empty());

    server.stop();
};

auto tReturnsBeforeTheReply = test("HttpAsync/returnsBeforeTheServerReplies") = []
{
    auto server = Server(threadPoolOptions());
    auto stalled = StallGate();

    auto handler = [&](const Request&) { return stallThenRespond(stalled, "slow"); };
    check(server.listen(0, handler));

    auto calls = 0;
    auto onResponse = [&](const Response&) { ++calls; };

    auto beforeTheReply = eacp::Time::Deadline {MS {200}};
    asyncRequest(Request(baseUrl(server.boundPort()) + "/slow"), onResponse);

    check(!beforeTheReply.expired());
    check(calls == 0);

    stalled.release();

    auto arrived = [&] { return calls > 0; };
    check(pumpUntil(arrived));

    server.stop();
};

auto tFailureArrives = test("HttpAsync/failureArrivesInTheResponse") = []
{
    auto received = Response();
    auto calls = 0;

    auto onResponse = [&](const Response& res)
    {
        received = res;
        ++calls;
    };

    asyncRequest(Request("http://127.0.0.1:9/nobody-listening"), onResponse);

    auto arrived = [&] { return calls > 0; };
    check(pumpUntil(arrived));

    check(calls == 1);
    check(!received.error.empty());
    check(received.statusCode == 0);
};

auto tHonoursTimeout = test("HttpAsync/honoursTheRequestTimeout") = []
{
    auto server = Server(threadPoolOptions());
    auto stalled = StallGate();

    auto handler = [&](const Request&) { return stallThenRespond(stalled, "late"); };
    check(server.listen(0, handler));

    auto req = Request(baseUrl(server.boundPort()) + "/slow");
    req.timeout = MS {250};

    auto received = Response();
    auto calls = 0;

    auto onResponse = [&](const Response& res)
    {
        received = res;
        ++calls;
    };

    auto beforeTheServerReplies = eacp::Time::Deadline {MS {1000}};
    asyncRequest(req, onResponse);

    auto arrived = [&] { return calls > 0; };
    check(pumpUntil(arrived));

    check(!beforeTheServerReplies.expired());
    check(!received.error.empty());

    stalled.release();
    server.stop();
};

auto tCancelAllStops = test("HttpAsync/cancelAllStopsTheCallback") = []
{
    auto server = Server(threadPoolOptions());
    auto stalled = StallGate();

    auto handler = [&](const Request&) { return stallThenRespond(stalled, "late"); };
    check(server.listen(0, handler));

    auto calls = 0;
    auto onResponse = [&](const Response&) { ++calls; };

    asyncRequest(Request(baseUrl(server.boundPort()) + "/slow"), onResponse);
    cancelAllAsyncRequests();

    // Let the reply the cancel raced actually land: the callback still must
    // not fire.
    stalled.release();
    pumpFor(MS {250});

    check(calls == 0);

    server.stop();
};

auto tDownloadWritesTheFile = test("HttpAsync/downloadWritesTheFile") = []
{
    auto server = Server();

    auto handler = [](const Request&) { return respondWith("payload-bytes"); };
    check(server.listen(0, handler));

    auto destination = tempPath("download");
    auto req = DownloadRequest(baseUrl(server.boundPort()) + "/file", destination);

    auto received = Response();
    auto calls = 0;

    auto onResponse = [&](const Response& res)
    {
        received = res;
        ++calls;
    };

    asyncDownload(req, onResponse);

    auto arrived = [&] { return calls > 0; };
    check(pumpUntil(arrived));

    check(calls == 1);
    check(received.error.empty());
    check(received.statusCode == 200);
    check(readFile(destination) == "payload-bytes");

    auto ec = std::error_code();
    std::filesystem::remove(destination, ec);

    server.stop();
};

auto tDownloadWithoutPath = test("HttpAsync/downloadWithoutAPathFails") = []
{
    auto req = DownloadRequest();
    req.url = "http://127.0.0.1:9/nobody-listening";

    auto received = Response();
    auto calls = 0;

    auto onResponse = [&](const Response& res)
    {
        received = res;
        ++calls;
    };

    asyncDownload(req, onResponse);

    auto arrived = [&] { return calls > 0; };
    check(pumpUntil(arrived));

    check(calls == 1);
    check(received.error == "No destination file path");
};
