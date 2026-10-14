#include "Common.h"
#include <eacp/Core/Utils/Zip.h>
#include <eacp/Network/HTTP/HttpProtocol.h>
#include <filesystem>
#include <fstream>
#include <mutex>
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
    auto path = std::filesystem::temp_directory_path() / ("eacp-resume-" + name);
    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
    return path.string();
}

void writeFile(const std::string& path, const std::string& bytes)
{
    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string readFile(const std::string& path)
{
    auto in = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void removeFile(const std::string& path)
{
    auto ec = std::error_code();
    std::filesystem::remove(path, ec);
}

std::string makeBody()
{
    auto body = std::string();

    for (auto i = 0; i < 1000; ++i)
        body.push_back((char) ('a' + i % 26));

    return body;
}

std::int64_t rangeStart(const std::string& range)
{
    auto dash = range.find('-');
    return std::stoll(range.substr(6, dash - 6));
}

// /range honours a Range header as a server should, /ignores answers every
// request whole, /misplaced answers a Range with a 206 that starts at zero,
// and /missing is a 404. Every Range header that arrives is recorded.
struct RangeServer
{
    RangeServer()
    {
        auto handler = [this](const Request& req) { return answer(req); };
        listening = server.listen(0, handler);
    }

    Response answer(const Request& req)
    {
        auto range = req.getHeader("Range");
        record(req.type, range, req.getHeader("Accept-Encoding"));

        auto path = req.pathWithoutQuery();

        if (path == "/missing")
            return status(404, "not here");

        if (path == "/encoded")
            return encodedAnswer(req, range);

        if (range.empty() || path == "/ignores")
            return status(200, body);

        if (path == "/misplaced")
            return partial(0);

        if (path == "/short")
            return partial(rangeStart(range), 100);

        auto start = rangeStart(range);

        if (start >= (std::int64_t) body.size())
        {
            auto res = status(416, {});
            res.headers["Content-Range"] = "bytes */" + std::to_string(body.size());
            return res;
        }

        return partial(start);
    }

    Response partial(std::int64_t start, std::int64_t length = -1) const
    {
        return partialOf(body, start, length);
    }

    static Response partialOf(const std::string& entity,
                              std::int64_t start,
                              std::int64_t length = -1)
    {
        auto size = (std::int64_t) entity.size();
        auto end = length < 0 ? size : std::min(size, start + length);
        auto res =
            status(206, entity.substr((size_t) start, (size_t) (end - start)));
        res.headers["Content-Range"] = "bytes " + std::to_string(start) + "-"
                                       + std::to_string(end - 1) + "/"
                                       + std::to_string(size);
        return res;
    }

    // A server that compresses when asked, and whose ranges - as RFC 9110 has
    // them - count bytes of the compressed representation it would send.
    Response encodedAnswer(const Request& req, const std::string& range) const
    {
        auto acceptsDeflate =
            eacp::Strings::toLower(req.getHeader("Accept-Encoding")).find("deflate")
            != std::string::npos;

        if (!acceptsDeflate)
            return range.empty() ? status(200, body) : partial(rangeStart(range));

        auto encoded = deflated();
        auto start = range.empty() ? 0 : rangeStart(range);

        if (start >= (std::int64_t) encoded.size())
            return status(416, {});

        auto res = range.empty() ? status(200, encoded) : partialOf(encoded, start);
        res.headers["Content-Encoding"] = "deflate";
        return res;
    }

    std::string deflated() const
    {
        auto bytes = eacp::Zip::compress(body);
        return {bytes.begin(), bytes.end()};
    }

    std::string acceptEncodingOfLastRequest()
    {
        auto lock = std::scoped_lock(mutex);
        return lastAcceptEncoding;
    }

    static Response status(int code, const std::string& content)
    {
        auto res = Response();
        res.statusCode = code;
        res.content = content;
        return res;
    }

    void record(const std::string& method,
                const std::string& range,
                const std::string& acceptEncoding)
    {
        auto lock = std::scoped_lock(mutex);
        methods.push_back(method);
        ranges.push_back(range);
        lastAcceptEncoding = acceptEncoding;
    }

    std::vector<std::string> rangesSeen()
    {
        auto lock = std::scoped_lock(mutex);
        return ranges;
    }

    std::vector<std::string> methodsSeen()
    {
        auto lock = std::scoped_lock(mutex);
        return methods;
    }

    std::string url(const std::string& path) const
    {
        return baseUrl(server.boundPort()) + path;
    }

    const std::string body = makeBody();
    Server server;
    bool listening = false;
    std::mutex mutex;
    std::vector<std::string> methods;
    std::vector<std::string> ranges;
    std::string lastAcceptEncoding;
};

Response
    downloadOffTheLoop(Server& server, const Request& req, const std::string& path)
{
    auto result = Response();
    auto worker = std::thread();

    auto startWorker = [&]
    {
        auto runAndStop = [&]
        {
            result = req.downloadTo(path);
            callAsync([] { stopEventLoop(); });
        };

        worker = std::thread(runAndStop);
    };

    check(eacp::Threads::runEventLoopFor(eacp::Time::MS {10000}, startWorker));

    if (worker.joinable())
        worker.join();

    server.stop();
    return result;
}

struct ResumeCase
{
    ResumeCase(const std::string& name, const std::string& existing)
        : path(tempPath(name))
    {
        check(host.listening);
        writeFile(path, existing);
    }

    ~ResumeCase() { removeFile(path); }

    Response download(const std::string& route, bool resume = true)
    {
        auto req = Request(host.url(route));
        req.resumeDownload = resume;
        req.progress = &progress;
        return downloadOffTheLoop(host.server, req, path);
    }

    RangeServer host;
    std::string path;
    DownloadProgress progress;
};
} // namespace

auto tOffByDefault =
    test("HttpResume/isOffByDefault") = [] { check(!Request().resumeDownload); };

auto tAppendsPartialContent =
    test("HttpResume/appendsA206AndCountsTheWholeFile") = []
{
    auto run = ResumeCase("append.bin", makeBody().substr(0, 400));

    auto res = run.download("/range");

    check(res.error.empty());
    check(res.statusCode == 200);
    check(readFile(run.path) == run.host.body);
    check(run.host.rangesSeen() == std::vector<std::string> {"bytes=400-"});
    check(run.progress.bytesReceived.load() == 1000);
    check(run.progress.totalBytes.load() == 1000);
    check(run.progress.done.load());
};

auto tReplacesOnFullResponse = test("HttpResume/replacesTheFileOnA200") = []
{
    auto run = ResumeCase("replace.bin", std::string(400, 'x'));

    auto res = run.download("/ignores");

    check(res.error.empty());
    check(res.statusCode == 200);
    check(readFile(run.path) == run.host.body);
    check(run.host.rangesSeen() == std::vector<std::string> {"bytes=400-"});
    check(run.progress.bytesReceived.load() == 1000);
};

auto tKeepsWholeFile = test("HttpResume/keepsAFileThatIsAlreadyWhole") = []
{
    auto run = ResumeCase("whole.bin", makeBody());

    auto res = run.download("/range");

    check(res.error.empty());
    check(res.statusCode == 200);
    check(readFile(run.path) == run.host.body);
    check(run.host.rangesSeen().size() == 1);
    check(run.progress.bytesReceived.load() == 1000);
    check(run.progress.totalBytes.load() == 1000);
};

auto tRestartsWhenTooLong =
    test("HttpResume/startsOverWhenTheFileIsLongerThanTheResource") = []
{
    auto run = ResumeCase("too-long.bin", makeBody() + "extra");

    auto res = run.download("/range");

    check(res.error.empty());
    check(res.statusCode == 200);
    check(readFile(run.path) == run.host.body);
    check(run.host.rangesSeen() == std::vector<std::string> {"bytes=1005-", ""});
};

auto tRestartsOnMisplacedRange =
    test("HttpResume/startsOverWhenThe206StartsElsewhere") = []
{
    auto run = ResumeCase("misplaced.bin", makeBody().substr(0, 400));

    auto res = run.download("/misplaced");

    check(res.error.empty());
    check(res.statusCode == 200);
    check(readFile(run.path) == run.host.body);
    check(run.host.rangesSeen() == std::vector<std::string> {"bytes=400-", ""});
    check(run.progress.bytesReceived.load() == 1000);
};

auto tKeepsPartialOnError = test("HttpResume/anErrorLeavesThePartialFile") = []
{
    auto partial = makeBody().substr(0, 400);
    auto run = ResumeCase("error.bin", partial);

    auto res = run.download("/missing");

    check(res.statusCode == 404);
    check(readFile(run.path) == partial);
};

auto tNoFileDownloadsWhole = test("HttpResume/noFileMeansAPlainDownload") = []
{
    auto run = ResumeCase("absent.bin", {});
    removeFile(run.path);

    auto res = run.download("/range");

    check(res.statusCode == 200);
    check(readFile(run.path) == run.host.body);
    check(run.host.rangesSeen() == std::vector<std::string> {""});
};

auto tWithoutResumeReplaces = test("HttpResume/withoutItTheFileIsReplaced") = []
{
    auto run = ResumeCase("plain.bin", std::string(400, 'x'));

    auto res = run.download("/range", false);

    check(res.statusCode == 200);
    check(readFile(run.path) == run.host.body);
    check(run.host.rangesSeen() == std::vector<std::string> {""});
};

auto tResumeIsNotSplit = test("HttpResume/aResumedDownloadIsNotSplit") = []
{
    auto run = ResumeCase("parallel.bin", makeBody().substr(0, 400));

    auto req = Request(run.host.url("/range"));
    req.resumeDownload = true;
    req.parallelChunks = 4;

    auto res = downloadOffTheLoop(run.host.server, req, run.path);

    check(res.statusCode == 200);
    check(readFile(run.path) == run.host.body);
    check(run.host.methodsSeen() == std::vector<std::string> {"GET"});
};

auto tShortRemainderIsAnError =
    test("HttpResume/aRemainderThatStopsShortIsAnError") = []
{
    auto run = ResumeCase("short.bin", makeBody().substr(0, 400));

    auto res = run.download("/short");

    check(!res.error.empty());
    check(res.statusCode == 0);
    check(readFile(run.path) == makeBody().substr(0, 500));
};

auto tResumeAsksForIdentity =
    test("HttpResume/asksForTheUnencodedRepresentation") = []
{
    auto run = ResumeCase("encoded.bin", makeBody().substr(0, 10));

    auto res = run.download("/encoded");

    check(res.error.empty());
    check(res.statusCode == 200);
    check(readFile(run.path) == run.host.body);
    check(run.host.rangesSeen() == std::vector<std::string> {"bytes=10-"});
    check(run.host.acceptEncodingOfLastRequest() == "identity");
};

auto tCallerEncodingWins = test("HttpResume/keepsTheCallersAcceptEncoding") = []
{
    auto run = ResumeCase("chosen.bin", makeBody().substr(0, 400));

    auto req = Request(run.host.url("/range"));
    req.resumeDownload = true;
    req.headers["Accept-Encoding"] = "br";

    downloadOffTheLoop(run.host.server, req, run.path);

    check(run.host.acceptEncodingOfLastRequest() == "br");
};
