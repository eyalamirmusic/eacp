#include "Common.h"

#include <eacp/Core/Utils/FileWriter.h>

#if defined(_WIN32)
#include <eacp/Core/Utils/WinInclude.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

using namespace nano;

namespace HttpStreamingBodyTests
{
using eacp::HTTP::Request;
using eacp::HTTP::RequestBodyReader;
using eacp::HTTP::Response;
using eacp::HTTP::Server;

constexpr auto mebibyte = std::int64_t {1024 * 1024};
constexpr auto largeBodyBytes = 160 * mebibyte;

// NSURLSession alone costs about 40 MiB the first time a process uploads,
// whatever the size; a body held whole would cost all 160.
constexpr auto largestAllowedPeakGrowth = 64 * mebibyte;
constexpr auto handlerReadSize = std::size_t {64 * 1024};
constexpr auto exchangeTimeout = eacp::Time::MS {60000};
constexpr auto statusOk = 200;
constexpr auto statusBadRequest = 400;
constexpr auto statusPayloadTooLarge = 413;
constexpr auto largestAcceptedBody = std::int64_t {1024};

#if defined(__APPLE__)
constexpr auto maxResidentSetUnitBytes = std::int64_t {1};
#else
constexpr auto maxResidentSetUnitBytes = std::int64_t {1024};
#endif

// The process's peak resident memory so far, which only ever grows: reading it
// before and after a transfer bounds what the transfer itself held at once.
std::int64_t getPeakResidentBytes()
{
#if defined(_WIN32)
    auto counters = PROCESS_MEMORY_COUNTERS {};
    K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return (std::int64_t) counters.PeakWorkingSetSize;
#else
    auto usage = rusage {};
    getrusage(RUSAGE_SELF, &usage);
    return (std::int64_t) usage.ru_maxrss * maxResidentSetUnitBytes;
#endif
}

char getPatternByteAt(std::int64_t offset)
{
    constexpr auto patternPeriod = 251;
    return (char) (offset % patternPeriod);
}

void writePatternFile(const std::string& path, std::int64_t size)
{
    auto writer = eacp::FileWriter(eacp::FilePath(path));
    auto piece = std::string((std::size_t) mebibyte, '\0');

    for (auto written = std::int64_t {0}; written < size; written += mebibyte)
    {
        for (auto i = std::int64_t {0}; i < mebibyte; ++i)
            piece[(std::size_t) i] = getPatternByteAt(written + i);

        writer.write(piece);
    }
}

struct BodyTally
{
    std::int64_t byteCount = 0;
    std::size_t largestPiece = 0;
    bool matchesPattern = true;
    std::optional<std::int64_t> declaredLength;
};

void tallyWholeBody(RequestBodyReader& body, BodyTally& tally)
{
    tally.declaredLength = body.getDeclaredLength();

    while (true)
    {
        auto piece = body.readSome(handlerReadSize);

        if (piece.empty())
            return;

        for (auto i = std::size_t {0}; i < piece.size(); ++i)
            if (piece[i] != getPatternByteAt(tally.byteCount + (std::int64_t) i))
                tally.matchesPattern = false;

        tally.byteCount += (std::int64_t) piece.size();
        tally.largestPiece = std::max(tally.largestPiece, piece.size());
    }
}

Response makeTextResponse(int statusCode, const std::string& text)
{
    auto response = Response();
    response.statusCode = statusCode;
    response.setContent(text, "text/plain");
    return response;
}

// The server reads headers on the event loop on Apple, so the client has to
// run off it while it spins.
void runClientWhileServing(Server& server, const std::function<void()>& client)
{
    auto worker = std::thread();

    eacp::Threads::runEventLoopFor(exchangeTimeout,
                                   [&]
                                   {
                                       worker = std::thread(
                                           [&]
                                           {
                                               client();
                                               eacp::Threads::callAsync(
                                                   [] { eacp::Threads::stopEventLoop(); });
                                           });
                                   });

    worker.join();
    server.stop();
}

std::string receiveUntilClosed(eacp::TCP::Connection& connection)
{
    auto received = std::string();

    while (true)
    {
        auto more = std::string();

        try
        {
            more = connection.receive();
        }
        catch (const eacp::TCP::Error&)
        {
            return received;
        }

        if (more.empty())
            return received;

        received.append(more);
    }
}

std::string exchangeRawBytes(Server& server, const std::string& requestBytes)
{
    auto reply = std::string();

    runClientWhileServing(server,
                          [&]
                          {
                              auto connection = eacp::TCP::Connection::connect(
                                  {"127.0.0.1", (std::uint16_t) server.boundPort()});
                              connection.send(requestBytes);
                              reply = receiveUntilClosed(connection);
                          });

    return reply;
}

std::string echoWholeBody(RequestBodyReader& body)
{
    auto whole = std::string();

    while (true)
    {
        auto piece = body.readSome(handlerReadSize);

        if (piece.empty())
            return whole;

        whole.append(piece);
    }
}

auto tLargeBodyStreamsFlat = test("HttpStreamingBody/largeBodyStreamsWithFlatMemory") = []
{
    auto bodyPath = tempPath("streaming-large-body.bin");
    writePatternFile(bodyPath, largeBodyBytes);

    auto tally = BodyTally();
    auto server = Server();
    server.addStreamingRoute("PUT",
                             "/upload",
                             [&](const Request&, RequestBodyReader& body)
                             {
                                 tallyWholeBody(body, tally);
                                 return makeTextResponse(statusOk, "stored");
                             });
    check(server.listen(0));

    auto request = Request(baseUrl(server.boundPort()) + "/upload");
    request.type = "PUT";
    request.bodyFilePath = bodyPath;

    auto peakBefore = getPeakResidentBytes();
    auto response = Response();
    runClientWhileServing(server, [&] { response = request.perform(); });
    auto peakGrowth = getPeakResidentBytes() - peakBefore;

    check(response.statusCode == statusOk);
    check(tally.byteCount == largeBodyBytes);
    check(tally.declaredLength == largeBodyBytes);
    check(tally.matchesPattern);
    check(tally.largestPiece <= handlerReadSize);
    check(peakGrowth < largestAllowedPeakGrowth);

    std::filesystem::remove(bodyPath);
};

auto tChunkedBody = test("HttpStreamingBody/chunkedBodyIsDecoded") = []
{
    auto received = std::string();
    auto declaredLength = std::optional<std::int64_t>(0);
    auto server = Server();
    server.addStreamingRoute("PUT",
                             "/upload",
                             [&](const Request&, RequestBodyReader& body)
                             {
                                 declaredLength = body.getDeclaredLength();
                                 received = echoWholeBody(body);
                                 return makeTextResponse(statusOk, "ok");
                             });
    check(server.listen(0));

    auto reply = exchangeRawBytes(server,
                                  "PUT /upload HTTP/1.1\r\n"
                                  "Transfer-Encoding: chunked\r\n\r\n"
                                  "5;name=value\r\nhello\r\n"
                                  "6\r\n world\r\n"
                                  "0\r\nTrailer: x\r\n\r\n");

    check(reply.starts_with("HTTP/1.1 200"));
    check(received == "hello world");
    check(!declaredLength.has_value());
};

auto tMalformedChunk = test("HttpStreamingBody/malformedChunkIsRejected") = []
{
    auto server = Server();
    server.addStreamingRoute("PUT",
                             "/upload",
                             [](const Request&, RequestBodyReader& body)
                             {
                                 echoWholeBody(body);
                                 return makeTextResponse(statusOk, "ok");
                             });
    check(server.listen(0));

    auto reply = exchangeRawBytes(server,
                                  "PUT /upload HTTP/1.1\r\n"
                                  "Transfer-Encoding: chunked\r\n\r\n"
                                  "zz\r\nhello\r\n0\r\n\r\n");

    check(reply.starts_with("HTTP/1.1 400"));
};

auto tInvalidContentLength = test("HttpStreamingBody/invalidContentLengthIsRejected") = []
{
    auto handlerCalled = false;
    auto server = Server();
    server.addStreamingRoute("PUT",
                             "/upload",
                             [&](const Request&, RequestBodyReader&)
                             {
                                 handlerCalled = true;
                                 return makeTextResponse(statusOk, "ok");
                             });
    check(server.listen(0));

    auto reply = exchangeRawBytes(
        server, "PUT /upload HTTP/1.1\r\nContent-Length: twelve\r\n\r\n");

    check(reply.starts_with("HTTP/1.1 400"));
    check(!handlerCalled);
};

// The client declares a body it never sends and waits for 100 Continue, so
// only a reply that reads nothing can reach it.
auto tRejectsEarly = test("HttpStreamingBody/rejectsBeforeReadingTheBody") = []
{
    auto server = Server();
    server.addStreamingRoute("PUT",
                             "/upload",
                             [](const Request&, RequestBodyReader& body)
                             {
                                 if (body.getDeclaredLength() > largestAcceptedBody)
                                     return makeTextResponse(statusPayloadTooLarge,
                                                             "too large");

                                 return makeTextResponse(statusOk, "ok");
                             });
    check(server.listen(0));

    auto reply = exchangeRawBytes(server,
                                  "PUT /upload HTTP/1.1\r\n"
                                  "Content-Length: 10000000000\r\n"
                                  "Expect: 100-continue\r\n\r\n");

    check(reply.starts_with("HTTP/1.1 413"));
    check(reply.find("100 Continue") == std::string::npos);
};

auto tExpectContinue = test("HttpStreamingBody/sendsContinueOnFirstRead") = []
{
    auto received = std::string();
    auto server = Server();
    server.addStreamingRoute("PUT",
                             "/upload",
                             [&](const Request&, RequestBodyReader& body)
                             {
                                 received = echoWholeBody(body);
                                 return makeTextResponse(statusOk, "ok");
                             });
    check(server.listen(0));

    auto interim = std::string();
    auto reply = std::string();

    runClientWhileServing(server,
                          [&]
                          {
                              auto connection = eacp::TCP::Connection::connect(
                                  {"127.0.0.1", (std::uint16_t) server.boundPort()});
                              connection.send("PUT /upload HTTP/1.1\r\n"
                                              "Content-Length: 5\r\n"
                                              "Expect: 100-continue\r\n\r\n");
                              interim = connection.receiveLine();
                              connection.receiveLine();
                              connection.send("hello");
                              reply = receiveUntilClosed(connection);
                          });

    check(interim == "HTTP/1.1 100 Continue");
    check(reply.starts_with("HTTP/1.1 200"));
    check(received == "hello");
};

auto tWildcardRoutes = test("HttpStreamingBody/wildcardRoutesMatchUnderTheirPrefix") = []
{
    auto server = Server();
    server.get("/objects/*",
               [](const Request& request)
               { return makeTextResponse(statusOk, "under " + request.pathWithoutQuery()); });
    server.get("/objects/a/*",
               [](const Request&) { return makeTextResponse(statusOk, "longer"); });
    server.get("/objects/exact",
               [](const Request&) { return makeTextResponse(statusOk, "exact"); });
    check(server.listen(0));

    auto base = baseUrl(server.boundPort());
    auto nested = Response();
    auto longer = Response();
    auto exact = Response();
    auto bare = Response();

    runClientWhileServing(server,
                          [&]
                          {
                              nested = Request(base + "/objects/b/parts/1").perform();
                              longer = Request(base + "/objects/a/parts").perform();
                              exact = Request(base + "/objects/exact").perform();
                              bare = Request(base + "/objects").perform();
                          });

    check(nested.content == "under /objects/b/parts/1");
    check(longer.content == "longer");
    check(exact.content == "exact");
    check(bare.statusCode == 404);
};

auto tBufferedRoutesBeside = test("HttpStreamingBody/bufferedRoutesStillReadWholeBodies") = []
{
    auto server = Server();
    server.post("/echo",
                [](const Request& request) { return makeTextResponse(statusOk, request.body); });
    server.addStreamingRoute("PUT",
                             "/echo",
                             [](const Request&, RequestBodyReader& body)
                             { return makeTextResponse(statusOk, echoWholeBody(body)); });
    check(server.listen(0));

    auto base = baseUrl(server.boundPort());
    auto buffered = Response();
    auto streamed = Response();

    runClientWhileServing(server,
                          [&]
                          {
                              buffered = Request::post(base + "/echo", "whole").perform();

                              auto put = Request::post(base + "/echo", "pieces");
                              put.type = "PUT";
                              streamed = put.perform();
                          });

    check(buffered.content == "whole");
    check(streamed.content == "pieces");
};

auto tFileResponse = test("HttpStreamingBody/responseBodyIsSentFromAFile") = []
{
    auto filePath = tempPath("streaming-response.bin");
    writePatternFile(filePath, mebibyte);

    auto server = Server();
    server.get("/file",
               [&](const Request&)
               {
                   auto response = Response();
                   response.statusCode = statusOk;
                   response.contentFilePath = filePath;
                   return response;
               });
    check(server.listen(0));

    auto response = Response();
    runClientWhileServing(
        server, [&] { response = Request(baseUrl(server.boundPort()) + "/file").perform(); });

    check(response.statusCode == statusOk);
    check(response.content == readFile(filePath));

    std::filesystem::remove(filePath);
};

} // namespace HttpStreamingBodyTests
