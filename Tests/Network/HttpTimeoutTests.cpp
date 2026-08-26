#include "Common.h"
#include <filesystem>
#include <system_error>
#include <thread>

using namespace nano;
using eacp::HTTP::DownloadProgress;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::HTTP::ServerOptions;
using eacp::HTTP::ServerThreadingMode;
using eacp::Threads::callAsync;
using eacp::Threads::stopEventLoop;
using eacp::Time::MS;

namespace
{
std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

std::string tempPath(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path() / ("eacp-timeout-" + name);
    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
    return path.string();
}

// The handlers here sleep, so they must not run on the event loop that the
// client's completion has to travel back through.
ServerOptions threadPoolOptions()
{
    auto options = ServerOptions();
    options.threading = ServerThreadingMode::ThreadPool;
    options.threadPoolSize = 2;
    return options;
}

Response sleepThenRespond(MS duration)
{
    eacp::Time::sleep(duration);

    auto res = Response();
    res.statusCode = 200;
    res.content = "ok";
    return res;
}

Response runOnWorkerThread(Server& server, const std::function<Response()>& perform)
{
    auto result = Response();
    auto worker = std::thread();

    auto startWorker = [&]
    {
        auto runRequest = [&]
        {
            result = perform();

            auto quit = [] { stopEventLoop(); };
            callAsync(quit);
        };

        worker = std::thread(runRequest);
    };

    eacp::Threads::runEventLoopFor(MS {10000}, startWorker);

    worker.join();
    server.stop();

    return result;
}
} // namespace

auto tDefaultIsNoLimit = test("HttpTimeout/defaultsToNoLimit") = []
{
    check(Request().timeout.count == 0);
    check(Request("http://example.com").timeout.count == 0);
    check(Request::post("http://example.com").timeout.count == 0);
};

auto tSlowResponseTimesOut = test("HttpTimeout/slowResponseFailsBeforeTheReply") = []
{
    auto server = Server(threadPoolOptions());

    auto handler = [](const Request&) { return sleepThenRespond(MS {1500}); };
    check(server.listen(0, handler));

    auto req = Request(baseUrl(server.boundPort()) + "/slow");
    req.timeout = MS {250};

    auto gaveUpEarly = false;

    auto perform = [&]
    {
        auto beforeTheServerReplies = eacp::Time::Deadline {MS {1000}};
        auto res = eacp::HTTP::httpRequest(req);
        gaveUpEarly = !beforeTheServerReplies.expired();
        return res;
    };

    auto res = runOnWorkerThread(server, perform);

    check(gaveUpEarly);
    check(!res.error.empty());
    check(res.statusCode == 0);
    check(res.content.empty());
};

auto tGenerousTimeoutCompletes =
    test("HttpTimeout/generousTimeoutStillCompletes") = []
{
    auto server = Server(threadPoolOptions());

    auto handler = [](const Request&) { return sleepThenRespond(MS {50}); };
    check(server.listen(0, handler));

    auto req = Request(baseUrl(server.boundPort()) + "/fast");
    req.timeout = MS {5000};

    auto perform = [&] { return eacp::HTTP::httpRequest(req); };
    auto res = runOnWorkerThread(server, perform);

    check(res.error.empty());
    check(res.statusCode == 200);
    check(res.content == "ok");
};

auto tDownloadTimesOut = test("HttpTimeout/downloadFailsWhenTheServerStalls") = []
{
    auto server = Server(threadPoolOptions());

    auto handler = [](const Request&) { return sleepThenRespond(MS {1500}); };
    check(server.listen(0, handler));

    auto progress = DownloadProgress();

    auto req = Request(baseUrl(server.boundPort()) + "/slow");
    req.timeout = MS {250};
    req.progress = &progress;

    auto destination = tempPath("stalled");
    auto gaveUpEarly = false;

    auto perform = [&]
    {
        auto beforeTheServerReplies = eacp::Time::Deadline {MS {1000}};
        auto res = req.downloadTo(destination);
        gaveUpEarly = !beforeTheServerReplies.expired();
        return res;
    };

    auto res = runOnWorkerThread(server, perform);

    check(gaveUpEarly);
    check(!res.error.empty());
    check(progress.done.load());

    auto ec = std::error_code();
    std::filesystem::remove(destination, ec);
};
