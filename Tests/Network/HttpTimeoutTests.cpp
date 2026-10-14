#include "Common.h"
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace nano;
using eacp::HTTP::DownloadProgress;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::Threads::callAsync;
using eacp::Threads::stopEventLoop;
using eacp::Time::MS;

namespace
{
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

// curl notices an idle transfer from its progress callback, which it calls
// about once a second while nothing moves, so an idle limit can fire up to a
// second late there. The bound these tests hold an idle failure to leaves
// room for that on top of a loaded runner's stalls.
constexpr auto withinTheIdleLimit = MS {5000};

// A client that never connects - a test that failed before its request -
// makes accept() give up and the server thread end, rather than hang.
eacp::TCP::Timeouts acceptTimeouts()
{
    auto timeouts = eacp::TCP::Timeouts();
    timeouts.connect = MS {5000};
    return timeouts;
}

// Sends its headers at once and then the body a piece at a time - or, told to
// stall, one piece and then nothing until the test releases it. HTTP::Server
// writes a response in one go, so trickling needs a server of its own.
struct TricklingServer
{
    TricklingServer(int piecesToSend, MS gapToUse, bool stallsToUse)
        : listener(eacp::TCP::Listener::bind(0, acceptTimeouts()))
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

Response responseOffTheLoop(const std::function<Response()>& perform)
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
        auto beforeTheServerReplies = eacp::Time::Deadline {withinTheIdleLimit};
        auto res = perform();
        outcome.gaveUpEarly = !beforeTheServerReplies.expired();
        server.stalled.release();
        return res;
    };

    outcome.response = responseOffTheLoop(timed);
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
    req.timeout = MS {50};

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
    req.timeout = MS {50};
    req.progress = &progress;

    auto destination = tempPath("timeout-stalled");
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
        auto beforeTheServerReplies = eacp::Time::Deadline {withinTheIdleLimit};
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
    auto res = responseOffTheLoop([&] { return req.perform(); });

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

    auto destination = tempPath("timeout-idle-stalled");
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

    auto destination = tempPath("timeout-idle-steady");
    auto res = responseOffTheLoop([&] { return req.downloadTo(destination); });

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

    auto res = responseOffTheLoop([&] { return req.perform(); });

    check(res.error.empty());
    check(res.statusCode == 200);
    check(res.content.size() == (size_t) server.bodySize());
};

auto tConnectFailsFast = test("HttpTimeout/connectTimeoutStillReportsRefusal") = []
{
    auto req = Request("http://127.0.0.1:1/refused");
    req.connectTimeout = MS {500};

    auto res = responseOffTheLoop([&] { return req.perform(); });

    check(!res.error.empty());
    check(res.statusCode == 0);
};

// WinHTTP sends the body a slice at a time under an idle limit, so this is a
// large upload going through that path and arriving whole.
auto tIdleUploadArrivesWhole =
    test("HttpTimeout/uploadUnderAnIdleLimitArrivesWhole") = []
{
    auto server = Server(threadPoolOptions());
    auto received = std::atomic<std::size_t> {0};

    auto handler = [&](const Request& req)
    {
        received = req.body.size();
        return okResponse();
    };
    check(server.listen(0, handler));

    auto req = Request::post(baseUrl(server.boundPort()) + "/upload",
                             std::string(3 * 1024 * 1024, 'u'));
    req.idleTimeout = MS {2000};

    auto res = runOnWorkerThread(server, [&] { return req.perform(); });

    check(res.error.empty());
    check(res.statusCode == 200);
    check(received == 3 * 1024 * 1024);
};

#if !defined(_WIN32)
namespace
{
// A listening socket whose accept queue is full drops the SYNs that follow,
// on macOS and Linux alike, so a connect to it hangs the way one to a
// blackholed address does - without needing such an address. Windows refuses
// instead, which is why this is POSIX only.
class FullBacklogListener
{
public:
    FullBacklogListener()
    {
        listening = ::socket(AF_INET, SOCK_STREAM, 0);

        auto address = loopback(0);
        ::bind(listening, (sockaddr*) &address, sizeof(address));
        ::listen(listening, 0);

        auto length = (socklen_t) sizeof(address);
        ::getsockname(listening, (sockaddr*) &address, &length);
        port = ntohs(address.sin_port);

        for (auto i = 0; i < 8; ++i)
            fillers.push_back(connectWithoutWaiting());

        eacp::Time::sleep(MS {200});
    }

    ~FullBacklogListener()
    {
        for (auto fd: fillers)
            ::close(fd);

        ::close(listening);
    }

    std::string url() const { return baseUrl(port) + "/never"; }

private:
    static sockaddr_in loopback(int portToUse)
    {
        auto address = sockaddr_in();
        address.sin_family = AF_INET;
        address.sin_port = htons((uint16_t) portToUse);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return address;
    }

    int connectWithoutWaiting() const
    {
        auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);

        auto address = loopback(port);
        ::connect(fd, (sockaddr*) &address, sizeof(address));
        return fd;
    }

    int listening = -1;
    int port = 0;
    std::vector<int> fillers;
};
} // namespace

auto tConnectTimeoutFires =
    test("HttpTimeout/connectTimeoutFiresOnAHungConnect") = []
{
    auto listener = FullBacklogListener();

    auto req = Request(listener.url());
    req.connectTimeout = MS {500};

    auto atLeastTheLimit = eacp::Time::Deadline {MS {400}};
    auto withinTheBound = eacp::Time::Deadline {withinTheIdleLimit};
    auto res = responseOffTheLoop([&] { return req.perform(); });

    check(!res.error.empty());
    check(res.statusCode == 0);
    check(atLeastTheLimit.expired());
    check(!withinTheBound.expired());
};
#endif
