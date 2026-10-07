#include "Common.h"
#include <atomic>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <thread>

using namespace nano;
using eacp::HTTP::DownloadProgress;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::Threads::callAsync;
using eacp::Threads::stopEventLoop;

namespace
{
constexpr auto limit = std::int64_t {16 * 1024};

std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

std::string tempPath(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path() / ("eacp-size-" + name);
    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
    return path.string();
}

bool serveBody(Server& server, const std::string& body)
{
    auto handler = [body](const Request&)
    {
        auto res = Response();
        res.statusCode = 200;
        res.content = body;
        return res;
    };

    return server.listen(0, handler);
}

bool runOffTheLoop(const std::function<void()>& work)
{
    auto worker = std::thread();

    auto startWorker = [&]
    {
        auto runAndStop = [&]
        {
            work();
            callAsync([] { stopEventLoop(); });
        };

        worker = std::thread(runAndStop);
    };

    auto stopped =
        eacp::Threads::runEventLoopFor(eacp::Time::MS {5000}, startWorker);

    if (worker.joinable())
        worker.join();

    return stopped;
}

Response performOffTheLoop(Server& server, const Request& req)
{
    auto response = Response();
    auto perform = [&] { response = req.perform(); };
    check(runOffTheLoop(perform));
    server.stop();
    return response;
}

void checkIsTooLarge(const Response& res)
{
    check(!res.error.empty());
    check(res.statusCode == 0);
    check(res.content.empty());
}

std::string chunk(const std::string& bytes)
{
    auto size = std::stringstream();
    size << std::hex << bytes.size();
    return size.str() + "\r\n" + bytes + "\r\n";
}

// HTTP::Server always sends a Content-Length, so a body whose size is only
// known once it has all arrived needs a server of its own.
struct ChunkedServer
{
    explicit ChunkedServer(int totalBytes)
        : listener(eacp::TCP::Listener::bind(0))
    {
        auto serve = [this, totalBytes] { serveOnce(totalBytes); };
        worker = std::thread(serve);
    }

    ~ChunkedServer()
    {
        if (worker.joinable())
            worker.join();
    }

    void serveOnce(int totalBytes)
    {
        try
        {
            auto peer = listener.accept();

            while (!peer.receiveLine().empty())
            {
            }

            peer.send("HTTP/1.1 200 OK\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n\r\n");

            auto piece = std::string(4096, 'c');

            for (auto sent = 0; sent < totalBytes; sent += (int) piece.size())
            {
                peer.send(chunk(piece));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            peer.send("0\r\n\r\n");
        }
        catch (const eacp::TCP::Error&)
        {
            abandoned = true;
        }
    }

    std::string url() const { return baseUrl(listener.port()) + "/chunked"; }

    eacp::TCP::Listener listener;
    std::thread worker;
    std::atomic<bool> abandoned {false};
};
} // namespace

auto tAtTheLimit = test("HttpResponseSize/bodyAtTheLimitArrives") = []
{
    auto server = Server();
    auto body = std::string((size_t) limit, 'a');
    check(serveBody(server, body));

    auto req = Request(baseUrl(server.boundPort()) + "/data");
    req.maxResponseSize = limit;

    auto res = performOffTheLoop(server, req);

    check(res.error.empty());
    check(res.statusCode == 200);
    check(res.content == body);
};

auto tOverTheLimit = test("HttpResponseSize/bodyOverTheLimitIsAnError") = []
{
    auto server = Server();
    check(serveBody(server, std::string(64 * 1024, 'b')));

    auto req = Request(baseUrl(server.boundPort()) + "/data");
    req.maxResponseSize = limit;

    checkIsTooLarge(performOffTheLoop(server, req));
};

auto tChunkedOverTheLimit =
    test("HttpResponseSize/chunkedBodyOverTheLimitIsAnError") = []
{
    auto server = ChunkedServer(256 * 1024);

    auto req = Request(server.url());
    req.maxResponseSize = limit;

    auto res = Response();
    auto perform = [&] { res = req.perform(); };
    check(runOffTheLoop(perform));

    checkIsTooLarge(res);
};

auto tChunkedWithoutLimit = test("HttpResponseSize/chunkedBodyArrivesWhole") = []
{
    auto server = ChunkedServer(64 * 1024);

    auto req = Request(server.url());

    auto res = Response();
    auto perform = [&] { res = req.perform(); };
    check(runOffTheLoop(perform));

    check(res.error.empty());
    check(res.statusCode == 200);
    check(res.content.size() == 64 * 1024);
};

auto tDownloadOverTheLimit =
    test("HttpResponseSize/downloadOverTheLimitIsAnError") = []
{
    auto server = Server();
    check(serveBody(server, std::string(64 * 1024, 'd')));

    auto progress = DownloadProgress();
    auto req = Request(baseUrl(server.boundPort()) + "/data");
    req.maxResponseSize = limit;
    req.progress = &progress;

    auto res = Response();
    auto path = tempPath("download.bin");
    auto download = [&] { res = req.downloadTo(path); };
    check(runOffTheLoop(download));
    server.stop();

    check(!res.error.empty());
    check(res.statusCode == 0);
    check(progress.done.load());
};

auto tZeroIsNoLimit = test("HttpResponseSize/zeroIsNoLimit") = []
{
    auto server = Server();
    auto body = std::string(64 * 1024, 'e');
    check(serveBody(server, body));

    auto req = Request(baseUrl(server.boundPort()) + "/data");

    auto res = performOffTheLoop(server, req);

    check(res.error.empty());
    check(res.statusCode == 200);
    check(res.content == body);
};
