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

std::string agentSeenDownloading(const Request& req)
{
    auto host = AgentRecordingServer();
    check(host.listening);

    auto sent = req;
    sent.url = host.url();

    auto path = tempPath("download.bin");
    auto res = Response();

    check(runOffTheLoop(host.server, [&] { res = sent.downloadTo(path); }));
    check(res.statusCode == 200);

    auto ec = std::error_code();
    std::filesystem::remove(path, ec);

    return host.lastAgent();
}

// What a request with no User-Agent of its own carries when no app-wide one
// is set: the system's on Apple platforms and Android, eacp's elsewhere.
void checkIsThePlatformDefault(const std::string& agent)
{
#if defined(__APPLE__)
    check(agent.find("CFNetwork") != std::string::npos);
    check(!agent.starts_with("eacp/"));
#elif defined(__ANDROID__)
    check(!agent.empty());
    check(!agent.starts_with("eacp/"));
#else
    check(agent == eacp::HTTP::eacpUserAgent());
#endif
}

constexpr auto appAgent = "Modalics/Test/1.0";

// Sets the app-wide User-Agent for one test, and always puts the platform's
// back so no other test sees it.
struct AppWideAgent
{
    explicit AppWideAgent(const std::string& agent)
    {
        eacp::HTTP::setDefaultUserAgent(agent);
    }

    ~AppWideAgent() { eacp::HTTP::setDefaultUserAgent({}); }
};
} // namespace

auto tEacpNamesItsVersion = test("HttpUserAgent/eacpAgentNamesItsVersion") = []
{
    auto agent = eacp::HTTP::eacpUserAgent();
    check(agent.starts_with("eacp"));
    check(agent.find(' ') == std::string::npos);
};

auto tDefaultIsThePlatformOne = test("HttpUserAgent/defaultIsThePlatformOne") = []
{
#if defined(__APPLE__) || defined(__ANDROID__)
    check(eacp::HTTP::defaultUserAgent().empty());
#else
    check(eacp::HTTP::defaultUserAgent() == eacp::HTTP::eacpUserAgent());
#endif
};

auto tDefaultIsSent = test("HttpUserAgent/platformDefaultIsSent") = []
{ checkIsThePlatformDefault(agentSeenFor(Request())); };

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

auto tPostSendsDefault = test("HttpUserAgent/postSendsThePlatformDefaultToo") = []
{ checkIsThePlatformDefault(agentSeenFor(Request::post({}, "body"))); };

auto tDownloadSendsDefault =
    test("HttpUserAgent/downloadSendsThePlatformDefaultToo") = []
{ checkIsThePlatformDefault(agentSeenDownloading(Request())); };

auto tAppWideIsReported = test("HttpUserAgent/appWideAgentIsTheDefault") = []
{
    auto scoped = AppWideAgent(appAgent);
    check(eacp::HTTP::defaultUserAgent() == appAgent);
};

auto tAppWideIsSent = test("HttpUserAgent/appWideAgentIsSent") = []
{
    auto scoped = AppWideAgent(appAgent);
    check(agentSeenFor(Request()) == appAgent);
};

auto tHeaderBeatsAppWide = test("HttpUserAgent/callersHeaderBeatsAppWideAgent") = []
{
    auto scoped = AppWideAgent(appAgent);

    auto req = Request();
    req.headers["User-Agent"] = "MyApp/2.1 (test)";
    check(agentSeenFor(req) == "MyApp/2.1 (test)");
};

auto tPostSendsAppWide = test("HttpUserAgent/postSendsAppWideAgent") = []
{
    auto scoped = AppWideAgent(appAgent);
    check(agentSeenFor(Request::post({}, "body")) == appAgent);
};

auto tDownloadSendsAppWide = test("HttpUserAgent/downloadSendsAppWideAgent") = []
{
    auto scoped = AppWideAgent(appAgent);
    check(agentSeenDownloading(Request()) == appAgent);
};

auto tDownloadHeaderBeatsAppWide =
    test("HttpUserAgent/downloadHeaderBeatsAppWideAgent") = []
{
    auto scoped = AppWideAgent(appAgent);

    auto req = Request();
    req.headers["User-Agent"] = "MyApp/2.1 (test)";
    check(agentSeenDownloading(req) == "MyApp/2.1 (test)");
};

auto tResetRestoresPlatform =
    test("HttpUserAgent/emptyAgentRestoresThePlatformOne") = []
{
    eacp::HTTP::setDefaultUserAgent(appAgent);
    check(agentSeenFor(Request()) == appAgent);

    eacp::HTTP::setDefaultUserAgent({});
    checkIsThePlatformDefault(agentSeenFor(Request()));
    checkIsThePlatformDefault(agentSeenFor(Request::post({}, "body")));
    checkIsThePlatformDefault(agentSeenDownloading(Request()));
};
