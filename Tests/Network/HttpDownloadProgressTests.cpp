#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Network/HTTP/Http.h>
#include <eacp/Network/HTTPServer/HttpServer.h>
#include <NanoTest/NanoTest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
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
std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

std::string tempPath(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path()
              / ("eacp-progress-" + name);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

struct DownloadOutcome
{
    Response response;
    bool eventLoopFinished = false;
};

void performDownload(Server& server,
                     const Request& clientRequest,
                     const std::string& destPath,
                     DownloadOutcome& out,
                     std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    auto worker = std::thread();
    auto stopped = eacp::Threads::runEventLoopFor(
        timeout,
        [&]
        {
            worker = std::thread(
                [&]
                {
                    out.response = clientRequest.downloadTo(destPath);
                    callAsync([] { stopEventLoop(); });
                });
        });

    if (worker.joinable())
        worker.join();

    out.eventLoopFinished = stopped;
    server.stop();
}
} // namespace

auto tDefaults =
    test("HttpDownloadProgress/defaultsAreZeroedExceptTotalBytes") = []
{
    auto p = DownloadProgress();
    check(p.bytesReceived.load() == 0);
    check(p.totalBytes.load() == -1);
    check(!p.cancel.load());
    check(!p.done.load());
};

auto tUpdatesOnSuccess =
    test("HttpDownloadProgress/setsBytesReceivedAndDoneOnSuccess") = []
{
    auto server = Server();
    auto body = std::string(4096, 'x');

    check(server.listen(0,
                        [&](const Request&)
                        {
                            auto res = Response();
                            res.statusCode = 200;
                            res.content = body;
                            return res;
                        }));
    auto port = server.boundPort();

    auto progress = DownloadProgress();
    auto req = Request(baseUrl(port) + "/data");
    req.progress = &progress;

    auto out = DownloadOutcome();
    performDownload(server, req, tempPath("success.bin"), out);

    check(out.eventLoopFinished);
    check(out.response.statusCode == 200);
    check(progress.done.load());
    check(progress.bytesReceived.load() == (std::int64_t) body.size());
};

auto tReportsContentLength =
    test("HttpDownloadProgress/reportsTotalBytesFromContentLength") = []
{
    auto server = Server();
    auto body = std::string(2048, 'y');

    check(server.listen(0,
                        [&](const Request&)
                        {
                            auto res = Response();
                            res.statusCode = 200;
                            res.content = body;
                            return res;
                        }));
    auto port = server.boundPort();

    auto progress = DownloadProgress();
    auto req = Request(baseUrl(port) + "/data");
    req.progress = &progress;

    auto out = DownloadOutcome();
    performDownload(server, req, tempPath("content-length.bin"), out);

    check(out.eventLoopFinished);
    check(progress.totalBytes.load() == (std::int64_t) body.size());
};

auto tDoneEvenWithoutProgressArg =
    test("HttpDownloadProgress/downloadSucceedsWithoutProgressPointer") = []
{
    auto server = Server();
    auto body = std::string(1024, 'z');

    check(server.listen(0,
                        [&](const Request&)
                        {
                            auto res = Response();
                            res.statusCode = 200;
                            res.content = body;
                            return res;
                        }));
    auto port = server.boundPort();

    auto req = Request(baseUrl(port) + "/data");

    auto out = DownloadOutcome();
    performDownload(server, req, tempPath("no-progress.bin"), out);

    check(out.eventLoopFinished);
    check(out.response.statusCode == 200);
};

auto tDoneOnError = test("HttpDownloadProgress/setsDoneEvenOnError") = []
{
    auto progress = DownloadProgress();
    auto req = Request("http://127.0.0.1:1/never");
    req.progress = &progress;

    auto res = req.downloadTo(tempPath("err.bin"));

    check(!res.error.empty());
    check(progress.done.load());
};

auto tCancelHaltsTransfer = test("HttpDownloadProgress/cancelHaltsTransfer") = []
{
    auto server = Server();
    auto body = std::string(4 * 1024 * 1024, 'q');

    check(server.listen(0,
                        [&](const Request&)
                        {
                            auto res = Response();
                            res.statusCode = 200;
                            res.content = body;
                            return res;
                        }));
    auto port = server.boundPort();

    auto progress = DownloadProgress();
    auto req = Request(baseUrl(port) + "/big");
    req.progress = &progress;

    auto out = DownloadOutcome();
    auto canceller = std::thread(
        [&]
        {
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (progress.bytesReceived.load() > 0)
                {
                    progress.cancel.store(true);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });

    performDownload(server,
                    req,
                    tempPath("cancel.bin"),
                    out,
                    std::chrono::seconds(10));
    canceller.join();

    check(progress.done.load());
    check(progress.cancel.load());
    check(progress.bytesReceived.load() < (std::int64_t) body.size());
    check(!out.response.error.empty());
};
