#include "Common.h"
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

using namespace nano;
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
    auto path = std::filesystem::temp_directory_path() / ("eacp-agent-" + name);
    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
    return path.string();
}

struct AgentRecordingServer
{
    AgentRecordingServer()
    {
        auto handler = [this](const Request& req) { return answer(req); };
        listening = server.listen(0, handler);
    }

    Response answer(const Request& req)
    {
        {
            auto lock = std::scoped_lock(mutex);
            agents.push_back(req.getHeader("User-Agent"));
        }

        auto res = Response();
        res.statusCode = 200;
        res.content = "ok";
        return res;
    }

    std::string lastAgent()
    {
        auto lock = std::scoped_lock(mutex);
        return agents.empty() ? std::string() : agents.back();
    }

    std::string url() const { return baseUrl(server.boundPort()) + "/agent"; }

    Server server;
    bool listening = false;
    std::mutex mutex;
    std::vector<std::string> agents;
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

std::string agentSeenFor(const Request& req)
{
    auto host = AgentRecordingServer();
    check(host.listening);

    auto sent = req;
    sent.url = host.url();

    auto res = Response();
    check(runOffTheLoop(host.server, [&] { res = sent.perform(); }));
    check(res.statusCode == 200);

    return host.lastAgent();
}
} // namespace

auto tDefaultNamesEacp = test("HttpUserAgent/defaultNamesEacpAndItsVersion") = []
{
    auto agent = eacp::HTTP::defaultUserAgent();
    check(agent.starts_with("eacp"));
    check(agent.find(' ') == std::string::npos);
};

auto tDefaultIsSent = test("HttpUserAgent/defaultIsSent") = []
{ check(agentSeenFor(Request()) == eacp::HTTP::defaultUserAgent()); };

auto tHeaderWins = test("HttpUserAgent/callersHeaderWins") = []
{
    auto req = Request();
    req.headers["User-Agent"] = "MyApp/2.1 (test)";
    check(agentSeenFor(req) == "MyApp/2.1 (test)");
};

auto tHeaderWinsInAnyCase = test("HttpUserAgent/callersHeaderWinsInAnyCase") = []
{
    auto req = Request();
    req.headers["user-agent"] = "lowercase/1";
    check(agentSeenFor(req) == "lowercase/1");
};

auto tPostSendsDefault = test("HttpUserAgent/postSendsTheDefaultToo") = []
{
    auto req = Request::post({}, "body");
    check(agentSeenFor(req) == eacp::HTTP::defaultUserAgent());
};

auto tDownloadSendsDefault = test("HttpUserAgent/downloadSendsTheDefaultToo") = []
{
    auto host = AgentRecordingServer();
    check(host.listening);

    auto req = Request(host.url());
    auto path = tempPath("download.bin");
    auto res = Response();

    check(runOffTheLoop(host.server, [&] { res = req.downloadTo(path); }));

    check(res.statusCode == 200);
    check(host.lastAgent() == eacp::HTTP::defaultUserAgent());

    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
};
