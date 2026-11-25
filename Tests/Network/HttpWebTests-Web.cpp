#include "WebCommon.h"

#include <eacp/Network/HTTP/HttpWeb.h>

#include <emscripten/em_js.h>

using namespace nano;
using eacp::HTTP::DownloadProgress;
using eacp::HTTP::Request;
using eacp::HTTP::Response;
using eacp::Threads::runEventLoopUntil;

namespace HttpWebTests
{
using namespace WebTests;

std::optional<Response> asyncResponse(const Request& req)
{
    auto response = std::optional<Response> {};
    eacp::HTTP::asyncRequest(req, [&](const Response& reply) { response = reply; });
    check(waitFor(response));
    return response;
}

std::optional<Response> asyncDownload(const Request& req, const std::string& path)
{
    auto download = eacp::HTTP::DownloadRequest {};
    static_cast<Request&>(download) = req;
    download.filePath = path;

    auto response = std::optional<Response> {};
    eacp::HTTP::asyncDownload(download,
                              [&](const Response& reply) { response = reply; });
    check(waitFor(response));
    return response;
}

auto tCanBlock =
    test("WebHttp/thisSuiteCanBlock") = [] { check(eacp::HTTP::webHttpCanBlock()); };

auto tGet = test("WebHttp/getReturnsStatusHeadersAndBody") = []
{
    auto response = Request {url("/get")}.perform();
    check(response.error.empty());
    check(response.statusCode == 200);
    check(response.content.starts_with("hello from "));
    check(eacp::HTTP::findHeaderIgnoringCase(response.headers, "X-Custom")
          == "custom-value");
};

auto tPostPut = test("WebHttp/postAndPutCarryTheirBodies") = []
{
    auto post = Request::post(url("/echo"), "payload-123");
    post.headers["X-Test"] = "abc";
    auto echo = echoOf(post.perform());
    check(echo.method == "POST");
    check(echo.body == "payload-123");
    check(echo.header("x-test") == "abc");

    auto put = Request {url("/echo")};
    put.type = "PUT";
    put.body = pattern(0, 100000);
    echo = echoOf(put.perform());
    check(echo.method == "PUT");
    check(echo.bodyLength == 100000);
};

auto tMultipart = test("WebHttp/multipartCarriesFieldsAndFiles") = []
{
    auto file = tempPath("web-multipart.txt");
    writeBytes(file, "file-on-disk");

    auto req = Request {url("/echo")};
    req.addFormField("field", "value-1");
    req.addFileBytes("bytes", "memory.bin", "bytes-in-memory");
    req.addFileField("file", file, "text/plain");

    for (const auto& response: {req.perform(), *asyncResponse(req)})
    {
        auto echo = echoOf(response);
        check(echo.method == "POST");
        check(echo.header("content-type").starts_with("multipart/form-data"));
        check(echo.body.find("value-1") != std::string::npos);
        check(echo.body.find("bytes-in-memory") != std::string::npos);
        check(echo.body.find("file-on-disk") != std::string::npos);
    }
};

auto tBodyFile = test("WebHttp/bodyFilePathIsUploaded") = []
{
    auto file = tempPath("web-upload.bin");
    writeBytes(file, pattern(7, 300000));

    auto req = Request {url("/echo")};
    req.type = "PUT";
    req.bodyFilePath = file;
    auto echo = echoOf(req.perform());
    check(echo.bodyLength == 300000);
    check(echo.header("content-length") == "300000");

    req.bodyFilePath = tempPath("web-upload-missing.bin");
    auto missing = req.perform();
    check(missing.statusCode == 0);
    check(!missing.error.empty());
};

auto tStatuses = test("WebHttp/errorStatusesAreResponses") = []
{
    for (auto code: {404, 500})
    {
        auto response =
            Request {url("/status?code=" + std::to_string(code))}.perform();
        check(response.statusCode == code);
        check(response.error.empty());
    }

    auto refused = Request {"http://127.0.0.1:1/"}.perform();
    check(refused.statusCode == 0);
    check(!refused.error.empty());
};

auto tGzip = test("WebHttp/gzipBodiesArriveDecoded") = []
{
    auto response = Request {url("/gzip")}.perform();
    check(response.statusCode == 200);
    check(response.content.size() == 10000);
    check(response.content.starts_with("gzip body"));
};

auto tReserved = test("WebHttp/headersTheRuntimeSetsAreNotForwarded") = []
{
    auto req = Request::post(url("/echo"), "12345");
    req.headers["Host"] = "elsewhere.example";
    req.headers["Content-Length"] = "3";
    req.headers["Connection"] = "close";
    auto response = req.perform();
    auto echo = echoOf(response);
    check(response.statusCode == 200);
    check(echo.header("host") == server().baseUrl.substr(7));
    check(echo.header("content-length") == "5");
    check(echo.body == "12345");
};

auto tAgent = test("WebHttp/defaultAndOwnUserAgentsReachTheServerUnderNode") = []
{
    check(eacp::HTTP::defaultUserAgent().empty());

    eacp::HTTP::setDefaultUserAgent("eacp-web-tests/1.0");
    auto echo = echoOf(Request {url("/echo")}.perform());
    eacp::HTTP::setDefaultUserAgent("");
    check(echo.header("user-agent") == "eacp-web-tests/1.0");

    auto own = Request {url("/echo")};
    own.headers["User-Agent"] = "own-agent";
    check(echoOf(own.perform()).header("user-agent") == "own-agent");
};

auto tCache = test("WebHttp/ignoringTheCacheAlwaysReachesTheServer") = []
{
    auto req = Request {url("/cacheable?key=web-tests")};
    auto first = req.perform();
    auto second = req.perform();
    check(first.content == "hits 1");
    check(second.content == "hits 2");

    req.cachePolicy = eacp::HTTP::CachePolicy::UseLocalCache;
    check(req.perform().statusCode == 200);
};

auto tSizeLimit = test("WebHttp/maxResponseSizeAbortsTheBody") = []
{
    auto declared = Request {url("/bytes?size=5000000")};
    declared.maxResponseSize = 1000000;
    auto response = declared.perform();
    check(response.statusCode == 0);
    check(response.error == eacp::HTTP::responseTooLargeError(1000000));

    auto undeclared = Request {url("/bytes?size=5000000&nolength=1")};
    undeclared.maxResponseSize = 1000000;
    response = undeclared.perform();
    check(response.error == eacp::HTTP::responseTooLargeError(1000000));

    auto small = Request {url("/bytes?size=1000")};
    small.maxResponseSize = 1000;
    check(small.perform().content.size() == 1000);
};

auto tCancel = test("WebHttp/cancelEndsAStalledRequest") = []
{
    auto progress = DownloadProgress {};
    auto req = Request {url("/stall?after=1000&ms=0")};
    req.progress = &progress;

    auto cancelledAt = Clock::time_point {};
    eacp::Threads::callAfter(eacp::Time::MS {300},
                             [&]
                             {
                                 cancelledAt = Clock::now();
                                 progress.cancel = true;
                             });

    auto response = req.perform();
    check(Clock::now() - cancelledAt < std::chrono::milliseconds(1000));
    check(response.statusCode == 0);
    check(response.error == "Download cancelled");
};

auto tAsync = test("WebHttp/asyncRequestAnswersOnALaterTurn") = []
{
    auto answered = false;
    auto response = Response {};

    eacp::HTTP::asyncRequest(Request::post(url("/echo"), "async-body"),
                             [&](const Response& reply)
                             {
                                 response = reply;
                                 answered = true;
                             });

    check(!answered);
    check(runEventLoopUntil([&] { return answered; }, fetchTimeout));
    check(echoOf(response).body == "async-body");
};

auto tOverlap = test("WebHttp/asyncRequestsOverlap") = []
{
    constexpr auto count = 6;
    auto finished = 0;
    auto succeeded = 0;
    auto start = Clock::now();

    for (auto i = 0; i < count; ++i)
        eacp::HTTP::asyncRequest(
            Request {url("/slow-headers?ms=500&n=" + std::to_string(i))},
            [&](const Response& reply)
            {
                succeeded += reply.content == "finally" ? 1 : 0;
                ++finished;
            });

    check(runEventLoopUntil([&] { return finished == count; }, fetchTimeout));
    check(succeeded == count);
    check(millisecondsSince(start) < 2500);
};

auto tCancelAll = test("WebHttp/cancelAllStopsTheCallbacks") = []
{
    auto answered = false;
    eacp::HTTP::asyncRequest(Request {url("/slow-headers?ms=300")},
                             [&](const Response&) { answered = true; });
    eacp::HTTP::cancelAllAsyncRequests();

    eacp::Threads::runEventLoopFor(eacp::Time::MS {600});
    check(!answered);
};

// A quit ends the pumps already running; a request made after it still waits
// for its answer, and the quit still ends the pump it was made under.
auto tAfterQuit = test("WebHttp/aBlockingRequestAfterQuitStillCompletes") = []
{
    auto response = std::optional<Response> {};

    eacp::Threads::callAsync(
        [&]
        {
            eacp::Threads::getEventLoop().quit();
            response = Request {url("/get")}.perform();
        });

    check(eacp::Threads::runEventLoopFor(fetchTimeout));
    check(response && response->statusCode == 200);
};

auto tTotal = test("WebHttpTimeout/totalTimeoutEndsTheRequest") = []
{
    auto req = Request {url("/slow-headers?ms=3000")};
    req.timeout = eacp::Time::MS {400};
    auto start = Clock::now();
    auto response = req.perform();
    check(response.error == eacp::HTTP::timedOutError());
    check(millisecondsSince(start) < 2000);
};

auto tConnect = test("WebHttpTimeout/connectTimeoutCoversTheWaitForHeaders") = []
{
    auto slowHeaders = Request {url("/slow-headers?ms=3000")};
    slowHeaders.connectTimeout = eacp::Time::MS {300};
    check(slowHeaders.perform().error == eacp::HTTP::timedOutError());

    auto slowBody = Request {url("/slow-body?ms=800")};
    slowBody.connectTimeout = eacp::Time::MS {300};
    auto response = slowBody.perform();
    check(response.statusCode == 200);
    check(response.content == "late body");
};

auto tIdle = test("WebHttpTimeout/idleTimeoutEndsAStallButNotASlowBody") = []
{
    auto stalled = Request {url("/stall?after=1000&ms=0")};
    stalled.idleTimeout = eacp::Time::MS {400};
    check(stalled.perform().error == eacp::HTTP::timedOutError());

    auto steady = Request {url("/drip?chunks=12&size=1000&ms=100")};
    steady.idleTimeout = eacp::Time::MS {500};
    auto response = steady.perform();
    check(response.statusCode == 200);
    check(response.content == pattern(0, 12000));
};

auto tChain = test("WebHttpRedirect/followsAChain") = []
{
    auto response = Request {url("/chain?n=3")}.perform();
    check(response.statusCode == 200);
    check(echoOf(response).path == "/echo");
};

auto tMethods = test("WebHttpRedirect/seeOtherBecomesGetAndTemporaryKeepsPost") = []
{
    auto seeOther = Request::post(url("/redirect?code=303&to=/echo"), "body");
    check(echoOf(seeOther.perform()).method == "GET");

    auto temporary = Request::post(url("/redirect?code=307&to=/echo"), "body");
    auto echo = echoOf(temporary.perform());
    check(echo.method == "POST");
    check(echo.body == "body");
};

// Node hands a redirect it was told not to follow back whole; a browser hides
// it (Apps/Web/HelloNetwork checks that side).
auto tNoFollow = test("WebHttpRedirect/notFollowingReturnsTheRedirectUnderNode") = []
{
    auto req = Request {url("/redirect?code=302&to=/get")};
    req.followRedirects = false;
    auto response = req.perform();
    check(response.statusCode == 302);
    check(eacp::HTTP::findHeaderIgnoringCase(response.headers, "Location")
          == "/get");
    check(response.content == "redirect body");
};

// Zero is checked on the redirect itself; any other limit is the runtime's
// own, which fetch() does not let a caller lower.
auto tLimit = test("WebHttpRedirect/zeroRedirectsFailsOnTheFirst") = []
{
    auto none = Request {url("/chain?n=3")};
    none.maxRedirects = 0;
    check(none.perform().error == eacp::HTTP::tooManyRedirectsError(0));

    auto plain = Request {url("/get")};
    plain.maxRedirects = 0;
    check(plain.perform().statusCode == 200);
};

auto tRuntimeLimit = test("WebHttpRedirect/aPositiveLimitIsTheRuntimesTwenty") = []
{
    auto twenty = Request {url("/chain?n=20")};
    twenty.maxRedirects = 1;
    auto followed = twenty.perform();
    check(followed.statusCode == 200);
    check(echoOf(followed).path == "/echo");

    auto twentyOne = Request {url("/chain?n=21")};
    twentyOne.maxRedirects = 1;
    auto refused = twentyOne.perform();
    check(refused.statusCode == 0);
    check(!refused.error.empty());
};

auto tCross = test("WebHttpRedirect/crossingOriginsDropsCredentials") = []
{
    auto req = Request {url("/cross")};
    req.headers["Authorization"] = "Bearer secret";
    auto response = req.perform();
    check(response.statusCode == 200);
    auto echo = echoOf(response);
    check(server().otherBaseUrl.ends_with(std::to_string(echo.port)));
    check(echo.header("authorization").empty());
};

auto tDownload = test("WebHttpDownload/downloadsToAFileWithProgress") = []
{
    auto path = tempPath("web-download.bin");
    auto progress = DownloadProgress {};
    auto req = Request {url("/bytes?size=1000000")};
    req.progress = &progress;

    auto response = req.downloadTo(path);
    check(response.statusCode == 200);
    check(readFile(path) == pattern(0, 1000000));
    check(progress.totalBytes == 1000000);
    check(progress.bytesReceived == 1000000);
    check(progress.done);
};

auto tAsyncDownload = test("WebHttpDownload/asyncDownloadWritesTheFile") = []
{
    auto path = tempPath("web-async-download.bin");
    auto response = asyncDownload(Request {url("/bytes?size=200000")}, path);
    check(response->statusCode == 200);
    check(readFile(path) == pattern(0, 200000));

    auto noPath = asyncDownload(Request {url("/get")}, {});
    check(!noPath->error.empty());
};

auto tEncoded = test("WebHttpDownload/anEncodedBodyHasNoKnownTotal") = []
{
    auto path = tempPath("web-download-gzip.txt");
    auto progress = DownloadProgress {};
    auto req = Request {url("/gzip")};
    req.progress = &progress;

    check(req.downloadTo(path).statusCode == 200);
    check(readFile(path).size() == 10000);
    check(progress.totalBytes == -1);
};

auto tResume = test("WebHttpDownload/resumeAppendsTheRest") = []
{
    auto path = tempPath("web-resume.bin");
    writeBytes(path, pattern(0, 40000));

    auto progress = DownloadProgress {};
    auto req = Request {url("/range?size=100000")};
    req.resumeDownload = true;
    req.progress = &progress;

    auto response = req.downloadTo(path);
    check(response.error.empty());
    check(response.statusCode == 200);
    check(readFile(path) == pattern(0, 100000));
    check(progress.bytesReceived == 100000);

    check(req.downloadTo(path).statusCode == 200);
    check(readFile(path) == pattern(0, 100000));
};

// The async path settles the ranged answer itself, and goes again without the
// Range when the file on disk cannot be the start of this one.
auto tAsyncResume = test("WebHttpDownload/asyncResumeAppendsOrStartsOver") = []
{
    auto path = tempPath("web-async-resume.bin");
    auto req = Request {url("/range?size=100000")};
    req.resumeDownload = true;

    writeBytes(path, pattern(0, 40000));
    check(asyncDownload(req, path)->statusCode == 200);
    check(readFile(path) == pattern(0, 100000));

    check(asyncDownload(req, path)->statusCode == 200);
    check(readFile(path) == pattern(0, 100000));

    writeBytes(path, pattern(0, 150000));
    auto restarted = asyncDownload(req, path);
    check(restarted->error.empty());
    check(restarted->statusCode == 200);
    check(readFile(path) == pattern(0, 100000));
};

auto tKeep = test("WebHttpDownload/anErrorStatusLeavesThePartialFile") = []
{
    auto path = tempPath("web-resume-404.bin");
    writeBytes(path, "partial");

    auto req = Request {url("/status?code=404")};
    req.resumeDownload = true;
    check(req.downloadTo(path).statusCode == 404);
    check(readFile(path) == "partial");
};

auto tParallel = test("WebHttpDownload/parallelChunksAreOneStream") = []
{
    auto req = Request {url("/range?size=2000000")};
    req.parallelChunks = 4;

    auto path = tempPath("web-parallel.bin");
    check(req.downloadTo(path).statusCode == 200);
    check(readFile(path) == pattern(0, 2000000));
};

auto tExit = test("WebHttp/theRuntimeExitingSilencesATransfer") = []
{
    auto req = Request {url("/slow-headers?ms=200")};
    req.timeout = eacp::Time::MS {1000};

    auto response = std::optional<Response> {};
    eacp::HTTP::asyncRequest(req, [&](const Response& reply) { response = reply; });
    runTheRuntimeExitCallbacks();

    check(waitFor(response));
    check(response->error == eacp::HTTP::timedOutError());
};
} // namespace HttpWebTests

EM_JS_DEPS(webTestsExitCallbacks, "$onExits");

EM_JS(void, runWebTestsExitCallbacks, (), {
    for (const callback of onExits)
        callback();
});

void WebTests::runTheRuntimeExitCallbacks()
{
    runWebTestsExitCallbacks();
}
