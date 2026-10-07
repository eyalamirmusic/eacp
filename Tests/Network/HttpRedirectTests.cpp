#include "Common.h"
#include <eacp/Network/HTTP/HttpProtocol.h>
#include <atomic>
#include <filesystem>
#include <system_error>
#include <thread>

using namespace nano;
using eacp::HTTP::asyncRequest;
using eacp::HTTP::findHeaderIgnoringCase;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::HTTP::Server;
using eacp::Threads::callAsync;
using eacp::Threads::runEventLoopUntil;
using eacp::Threads::stopEventLoop;

namespace
{
std::string baseUrl(int port)
{
    return "http://127.0.0.1:" + std::to_string(port);
}

std::string tempPath(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path() / ("eacp-redirect-" + name);
    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
    return path.string();
}

struct RedirectingServer
{
    RedirectingServer()
    {
        auto handler = [this](const Request& req) { return answer(req); };
        listening = server.listen(0, handler);
    }

    Response answer(const Request& req)
    {
        auto res = Response();

        if (req.pathWithoutQuery() == "/start")
        {
            res.setRedirect("/landing");
            res.content = "moved";
            return res;
        }

        ++landingHits;

        if (req.hasHeader("X-API-Key"))
            ++landingKeysSeen;

        res.statusCode = 200;
        res.content = "landed";
        return res;
    }

    std::string startUrl() const { return baseUrl(server.boundPort()) + "/start"; }

    Server server;
    bool listening = false;
    std::atomic<int> landingHits {0};
    std::atomic<int> landingKeysSeen {0};
};

bool runOffTheLoop(Server& server, const std::function<void()>& work)
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

    server.stop();
    return stopped;
}

Response performOffTheLoop(Server& server, const Request& req)
{
    auto response = Response();
    auto perform = [&] { response = req.perform(); };
    check(runOffTheLoop(server, perform));
    return response;
}
} // namespace

auto tFollowedByDefault = test("HttpRedirect/isFollowedByDefault") = []
{
    auto host = RedirectingServer();
    check(host.listening);

    auto res = performOffTheLoop(host.server, Request(host.startUrl()));

    check(res.error.empty());
    check(res.statusCode == 200);
    check(res.content == "landed");
    check(host.landingHits.load() == 1);
};

auto tNotFollowedReturnsRedirect =
    test("HttpRedirect/notFollowedReturnsTheRedirectItself") = []
{
    auto host = RedirectingServer();
    check(host.listening);

    auto req = Request(host.startUrl());
    req.followRedirects = false;

    auto res = performOffTheLoop(host.server, req);

    check(res.error.empty());
    check(res.statusCode == 302);
    check(findHeaderIgnoringCase(res.headers, "Location").ends_with("/landing"));
    check(res.content == "moved");
};

auto tNotFollowedSendsNothing =
    test("HttpRedirect/notFollowedSendsNothingToTheNewUrl") = []
{
    auto host = RedirectingServer();
    check(host.listening);

    auto req = Request(host.startUrl());
    req.headers["X-API-Key"] = "secret";
    req.followRedirects = false;

    auto res = performOffTheLoop(host.server, req);

    check(res.statusCode == 302);
    check(host.landingHits.load() == 0);
    check(host.landingKeysSeen.load() == 0);
};

auto tDownloadToHonours = test("HttpRedirect/downloadToHonoursIt") = []
{
    auto host = RedirectingServer();
    check(host.listening);

    auto req = Request(host.startUrl());
    req.followRedirects = false;

    auto path = tempPath("download.bin");
    auto res = Response();
    auto download = [&] { res = req.downloadTo(path); };
    check(runOffTheLoop(host.server, download));

    check(res.error.empty());
    check(res.statusCode == 302);
    check(host.landingHits.load() == 0);
};

auto tAsyncHonours = test("HttpRedirect/asyncRequestHonoursIt") = []
{
    auto host = RedirectingServer();
    check(host.listening);

    auto req = Request(host.startUrl());
    req.followRedirects = false;

    auto received = Response();
    auto calls = 0;

    auto onResponse = [&](const Response& res)
    {
        received = res;
        ++calls;
    };

    asyncRequest(req, onResponse);

    auto arrived = [&] { return calls > 0; };
    check(runEventLoopUntil(arrived, eacp::Time::MS {5000}));

    check(received.statusCode == 302);
    check(host.landingHits.load() == 0);

    host.server.stop();
};
