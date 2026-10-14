#include "Common.h"
#include <atomic>
#include <filesystem>
#include <system_error>
#include <thread>

using namespace nano;
using eacp::HTTP::CachePolicy;
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
    auto path = std::filesystem::temp_directory_path() / ("eacp-cache-" + name);
    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
    return path.string();
}

// Answers with everything a cache needs to keep the response and to
// revalidate it, and counts what reaches it.
struct CacheableServer
{
    CacheableServer()
    {
        auto handler = [this](const Request& req) { return answer(req); };
        listening = server.listen(0, handler);
    }

    Response answer(const Request& req)
    {
        ++hits;

        if (req.hasHeader("If-None-Match") || req.hasHeader("If-Modified-Since"))
            ++conditionalHits;

        auto res = Response();
        res.statusCode = 200;
        res.content = "cacheable";
        res.headers["Cache-Control"] = "public, max-age=3600";
        res.headers["ETag"] = "\"v1\"";
        res.headers["Last-Modified"] = "Mon, 01 Jan 2024 00:00:00 GMT";
        return res;
    }

    std::string url(const std::string& path) const
    {
        return baseUrl(server.boundPort()) + path;
    }

    Server server;
    bool listening = false;
    std::atomic<int> hits {0};
    std::atomic<int> conditionalHits {0};
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
        eacp::Threads::runEventLoopFor(eacp::Time::MS {10000}, startWorker);

    if (worker.joinable())
        worker.join();

    server.stop();
    return stopped;
}
} // namespace

auto tIgnoresCacheByDefault = test("HttpCache/defaultIgnoresTheLocalCache") = []
{ check(Request().cachePolicy == CachePolicy::IgnoreLocalCache); };

auto tRepeatedGetReachesServer =
    test("HttpCache/repeatedGetReachesTheServerEveryTime") = []
{
    auto host = CacheableServer();
    check(host.listening);

    auto first = Response();
    auto second = Response();

    auto twice = [&]
    {
        auto req = Request(host.url("/cacheable"));
        first = req.perform();
        second = req.perform();
    };

    check(runOffTheLoop(host.server, twice));

    check(first.statusCode == 200);
    check(second.statusCode == 200);
    check(second.content == "cacheable");
    check(host.hits.load() == 2);
    check(host.conditionalHits.load() == 0);
};

auto tRepeatedDownloadReachesServer =
    test("HttpCache/repeatedDownloadReachesTheServerEveryTime") = []
{
    auto host = CacheableServer();
    check(host.listening);

    auto path = tempPath("download.bin");
    auto last = Response();

    auto twice = [&]
    {
        auto req = Request(host.url("/download"));
        req.downloadTo(path);
        last = req.downloadTo(path);
    };

    check(runOffTheLoop(host.server, twice));

    check(last.statusCode == 200);
    check(host.hits.load() == 2);
    check(host.conditionalHits.load() == 0);

    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
};

// Whether a second request is answered locally is the platform cache's call,
// so only the request's success is the same everywhere.
auto tOptingInStillWorks = test("HttpCache/optingInToTheCacheStillAnswers") = []
{
    auto host = CacheableServer();
    check(host.listening);

    auto res = Response();

    auto twice = [&]
    {
        auto req = Request(host.url("/opt-in"));
        req.cachePolicy = CachePolicy::UseLocalCache;
        req.perform();
        res = req.perform();
    };

    check(runOffTheLoop(host.server, twice));

    check(res.error.empty());
    check(res.statusCode == 200);
    check(res.content == "cacheable");
    check(host.hits.load() >= 1);
};
