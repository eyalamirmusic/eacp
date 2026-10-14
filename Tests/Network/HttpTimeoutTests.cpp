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

Response okResponse()
{
    auto res = Response();
    res.statusCode = 200;
    res.content = "ok";
    return res;
}

Response sleepThenRespond(MS duration)
{
    eacp::Time::sleep(duration);
    return okResponse();
}

Response stallThenRespond(StallGate& gate)
{
    gate.wait();
    return okResponse();
}

// Sends its headers at once and then the body a piece at a time - or, told to
// stall, one piece and then nothing until the test releases it. HTTP::Server
// writes a response in one go, so trickling needs a server of its own.
struct TricklingServer
{
    TricklingServer(int piecesToSend, MS gapToUse, bool stallsToUse)
        : listener(eacp::TCP::Listener::bind(0))
        , pieces(piecesToSend)
        , gap(gapToUse)
        , stalls(stallsToUse)
    {
        worker = std::thread([this] { serveOnce(); });
    }

    ~TricklingServer()
    {
        stalled.release();

        if (worker.joinable())
            worker.join();
    }

    void serveOnce()
    {
        try
        {
            auto peer = listener.accept();

            while (!peer.receiveLine().empty())
            {
            }

            peer.send("HTTP/1.1 200 OK\r\nContent-Length: "
                      + std::to_string(bodySize())
                      + "\r\nConnection: close\r\n\r\n");

            for (auto i = 0; i < pieces; ++i)
            {
                if (stalls && i == 1)
                {
                    stalled.wait();
                    return;
                }

                peer.send(piece());
                eacp::Time::sleep(gap);
            }
        }
        catch (const eacp::TCP::Error&)
        {
        }
    }

    static std::string piece() { return std::string(1024, 't'); }

    int bodySize() const { return pieces * (int) piece().size(); }

    std::string url() const { return baseUrl(listener.port()) + "/trickle"; }

    eacp::TCP::Listener listener;
    int pieces;
    MS gap;
    bool stalls;
    StallGate stalled;
    std::thread worker;
};

Response runOffTheLoop(const std::function<Response()>& perform)
{
    auto result = Response();
    auto worker = std::thread();

    auto startWorker = [&]
    {
        auto runRequest = [&]
        {
            result = perform();
            callAsync([] { stopEventLoop(); });
        };

        worker = std::thread(runRequest);
    };

    eacp::Threads::runEventLoopFor(MS {10000}, startWorker);
    worker.join();

    return result;
}

struct TimedOutcome
{
    Response response;
    bool gaveUpEarly = false;
};

TimedOutcome performAgainstStall(TricklingServer& server,
                                 const std::function<Response()>& perform)
{
    auto outcome = TimedOutcome();

    auto timed = [&]
    {
        auto beforeTheServerReplies =
            eacp::Time::Deadline {withoutWaitingForTheServer};
        auto res = perform();
        outcome.gaveUpEarly = !beforeTheServerReplies.expired();
        server.stalled.release();
        return res;
    };

    outcome.response = runOffTheLoop(timed);
    return outcome;
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
    auto stalled = StallGate();

    auto handler = [&](const Request&) { return stallThenRespond(stalled); };
    check(server.listen(0, handler));

    auto req = Request(baseUrl(server.boundPort()) + "/slow");
    req.timeout = MS {250};

    auto gaveUpEarly = false;

    auto perform = [&]
    {
        auto beforeTheServerReplies =
            eacp::Time::Deadline {withoutWaitingForTheServer};
        auto res = eacp::HTTP::httpRequest(req);
        gaveUpEarly = !beforeTheServerReplies.expired();
        stalled.release();
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
    auto stalled = StallGate();

    auto handler = [&](const Request&) { return stallThenRespond(stalled); };
    check(server.listen(0, handler));

    auto progress = DownloadProgress();

    auto req = Request(baseUrl(server.boundPort()) + "/slow");
    req.timeout = MS {250};
    req.progress = &progress;

    auto destination = tempPath("stalled");
    auto gaveUpEarly = false;

    auto perform = [&]
    {
        auto beforeTheServerReplies =
            eacp::Time::Deadline {withoutWaitingForTheServer};
        auto res = req.downloadTo(destination);
        gaveUpEarly = !beforeTheServerReplies.expired();
        stalled.release();
        return res;
    };

    auto res = runOnWorkerThread(server, perform);

    check(gaveUpEarly);
    check(!res.error.empty());
    check(progress.done.load());

    auto ec = std::error_code();
    std::filesystem::remove(destination, ec);
};

auto tPhaseLimitsDefaultToUnset = test("HttpTimeout/phaseLimitsDefaultToUnset") = []
{
    auto req = Request();
    check(req.connectTimeout.count == 0);
    check(req.idleTimeout.count == 0);
};

auto tIdleFiresOnStall = test("HttpTimeout/idleTimeoutFiresWhenTheBodyStalls") = []
{
    auto server = TricklingServer(4, MS {0}, true);

    auto req = Request(server.url());
    req.idleTimeout = MS {300};

    auto outcome = performAgainstStall(server, [&] { return req.perform(); });

    check(outcome.gaveUpEarly);
    check(!outcome.response.error.empty());
    check(outcome.response.statusCode == 0);
};

auto tIdleFiresBeforeHeaders =
    test("HttpTimeout/idleTimeoutFiresWhenNoResponseArrives") = []
{
    auto server = Server(threadPoolOptions());
    auto stalled = StallGate();

    auto handler = [&](const Request&) { return stallThenRespond(stalled); };
    check(server.listen(0, handler));

    auto req = Request(baseUrl(server.boundPort()) + "/silent");
    req.idleTimeout = MS {300};

    auto gaveUpEarly = false;

    auto perform = [&]
    {
        auto beforeTheServerReplies =
            eacp::Time::Deadline {withoutWaitingForTheServer};
        auto res = req.perform();
        gaveUpEarly = !beforeTheServerReplies.expired();
        stalled.release();
        return res;
    };

    auto res = runOnWorkerThread(server, perform);

    check(gaveUpEarly);
    check(!res.error.empty());
};

auto tIdleSparesSteadyResponse =
    test("HttpTimeout/idleTimeoutSparesASlowButSteadyResponse") = []
{
    auto server = TricklingServer(12, MS {100}, false);

    auto req = Request(server.url());
    req.idleTimeout = MS {1000};

    auto started = eacp::Time::Deadline {MS {1000}};
    auto res = runOffTheLoop([&] { return req.perform(); });

    check(res.error.empty());
    check(res.statusCode == 200);
    check(res.content.size() == (size_t) server.bodySize());
    check(started.expired());
};

auto tIdleFiresOnStalledDownload =
    test("HttpTimeout/idleTimeoutFiresWhenADownloadStalls") = []
{
    auto server = TricklingServer(4, MS {0}, true);

    auto progress = DownloadProgress();
    auto req = Request(server.url());
    req.idleTimeout = MS {300};
    req.progress = &progress;

    auto destination = tempPath("idle-stalled");
    auto outcome =
        performAgainstStall(server, [&] { return req.downloadTo(destination); });

    check(outcome.gaveUpEarly);
    check(!outcome.response.error.empty());
    check(progress.done.load());

    auto ec = std::error_code();
    std::filesystem::remove(destination, ec);
};

auto tIdleSparesSteadyDownload =
    test("HttpTimeout/idleTimeoutSparesASlowButSteadyDownload") = []
{
    auto server = TricklingServer(12, MS {100}, false);

    auto progress = DownloadProgress();
    auto req = Request(server.url());
    req.idleTimeout = MS {1000};
    req.progress = &progress;

    auto destination = tempPath("idle-steady");
    auto res = runOffTheLoop([&] { return req.downloadTo(destination); });

    check(res.error.empty());
    check(res.statusCode == 200);
    check(progress.bytesReceived.load() == server.bodySize());

    auto ec = std::error_code();
    check(std::filesystem::file_size(destination, ec)
          == (uintmax_t) server.bodySize());
    std::filesystem::remove(destination, ec);
};

// Connecting to loopback is immediate, so this pins down the other half: a
// connect limit far shorter than the transfer does not cut a response short
// once it is flowing. Making a connect hang on purpose takes a blackholed
// address, which no runner can promise.
auto tConnectSparesLongTransfer =
    test("HttpTimeout/connectTimeoutDoesNotLimitTheTransfer") = []
{
    auto server = TricklingServer(8, MS {100}, false);

    auto req = Request(server.url());
    req.connectTimeout = MS {300};

    auto res = runOffTheLoop([&] { return req.perform(); });

    check(res.error.empty());
    check(res.statusCode == 200);
    check(res.content.size() == (size_t) server.bodySize());
};

auto tConnectFailsFast = test("HttpTimeout/connectTimeoutStillReportsRefusal") = []
{
    auto req = Request("http://127.0.0.1:1/refused");
    req.connectTimeout = MS {500};

    auto res = runOffTheLoop([&] { return req.perform(); });

    check(!res.error.empty());
    check(res.statusCode == 0);
};
