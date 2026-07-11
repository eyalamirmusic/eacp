#include "Common.h"
#include <filesystem>
#include <system_error>

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
    auto path = std::filesystem::temp_directory_path() / ("eacp-progress-" + name);
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

auto tDefaults = test("HttpDownloadProgress/defaultsAreZeroedExceptTotalBytes") = []
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
    // Pre-set cancel before the request runs — deterministic, no
    // race against transfer speed. Verifies the framework consults
    // the flag without depending on when the canceller thread
    // observes mid-flight bytes (which is unreliable on fast
    // loopback / CI runners).
    auto server = Server();
    auto body = std::string(4096, 'q');

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
    progress.cancel.store(true);

    auto req = Request(baseUrl(port) + "/big");
    req.progress = &progress;

    auto out = DownloadOutcome();
    performDownload(
        server, req, tempPath("cancel.bin"), out, std::chrono::seconds(5));

    check(progress.done.load());
    check(progress.cancel.load());
    check(!out.response.error.empty());
};
