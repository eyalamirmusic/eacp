#include "HttpProtocol.h"
#include "HttpWeb.h"

#include <eacp/Core/Threads/EventLoop-Web.h>
#include <eacp/Core/Threads/Timer.h>

#include <emscripten/emscripten.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>

// Http-Web.js runs each fetch()'s reader loop and pushes its head, body chunks
// and end into the eacp_web_http_* functions below, keyed by transfer id. A
// Timer, alive only while transfers are, checks cancel and the time limits.

extern "C"
{
    void eacp_web_http_start(int id,
                             const char* url,
                             const char* method,
                             const char* headers,
                             const void* body,
                             int bodyLength,
                             const char* bodyFilePath,
                             const char* redirect,
                             const char* cache);
    void eacp_web_http_abort(int id);
}

namespace eacp::HTTP
{
namespace
{
using WebHttpClock = std::chrono::steady_clock;

constexpr auto webHttpCheckInterval = Time::MS {50};

struct WebHttpFileCloser
{
    void operator()(std::FILE* file) const { std::fclose(file); }
};

using WebHttpFile = std::unique_ptr<std::FILE, WebHttpFileCloser>;

// filePath empty keeps the body in memory.
struct WebHttpTransfer
{
    Request request;
    WebHttpDone done = [](const Response&) {};
    std::string filePath;
    std::int64_t resumeFrom = 0;

    Response response;
    WebHttpFile file;
    ResponseSizeLimit limit;
    std::int64_t received = 0;
    bool headReceived = false;
    WebHttpClock::time_point started = WebHttpClock::now();
    WebHttpClock::time_point lastActivity = WebHttpClock::now();
};

struct WebHttpTransfers
{
    std::map<int, OwningPointer<WebHttpTransfer>> live;
    std::optional<Threads::Timer> checker;
    int nextId = 1;
};

WebHttpTransfers& webHttpTransfers()
{
    static auto transfers = WebHttpTransfers {};
    return transfers;
}

WebHttpTransfer* findWebHttpTransfer(int id)
{
    auto& live = webHttpTransfers().live;
    auto found = live.find(id);
    return found == live.end() ? nullptr : found->second.get();
}

Response webHttpFailure(const std::string& error)
{
    auto response = Response {};
    response.error = error;
    return response;
}

void finishWebHttp(int id, Response response)
{
    auto& transfers = webHttpTransfers();
    auto found = transfers.live.find(id);

    if (found == transfers.live.end())
        return;

    auto done = std::move(found->second->done);
    transfers.live.erase(found);

    if (transfers.live.empty())
        transfers.checker.reset();

    Threads::callAsync([done = std::move(done), response = std::move(response)]
                       { done(response); });
}

// Ends a transfer the JS side may still be running.
void failWebHttp(int id, const std::string& error)
{
    eacp_web_http_abort(id);
    finishWebHttp(id, webHttpFailure(error));
}

bool hasWebHttpLimitPassed(WebHttpClock::duration elapsed, Time::MS limit)
{
    return limit.count > 0 && elapsed >= std::chrono::milliseconds(limit.count);
}

std::string webHttpReasonToGiveUp(const WebHttpTransfer& transfer)
{
    auto now = WebHttpClock::now();
    const auto& req = transfer.request;

    if (req.progress != nullptr && req.progress->cancel.load())
        return "Download cancelled";

    if (hasWebHttpLimitPassed(now - transfer.started, req.timeout))
        return timedOutError();

    if (!transfer.headReceived
        && hasWebHttpLimitPassed(now - transfer.started, connectLimit(req)))
        return timedOutError();

    if (transfer.headReceived
        && hasWebHttpLimitPassed(now - transfer.lastActivity, idleLimit(req)))
        return timedOutError();

    return {};
}

void checkWebHttpTransfers()
{
    auto endings = std::map<int, std::string> {};

    for (const auto& [id, transfer]: webHttpTransfers().live)
        if (auto reason = webHttpReasonToGiveUp(*transfer); !reason.empty())
            endings[id] = reason;

    for (const auto& [id, reason]: endings)
        failWebHttp(id, reason);
}

bool isWebHttpHeaderTheRuntimeSets(const std::string& name)
{
    for (auto* reserved: {"Host", "Content-Length", "Connection"})
        if (Strings::equalsCaseInsensitive(name, reserved))
            return true;

    return false;
}

std::string webHttpHeaderLines(const Request& req)
{
    auto lines = std::string();

    for (const auto& [name, value]: req.headers)
        if (!isWebHttpHeaderTheRuntimeSets(name))
            lines.append(name).append(":").append(value).append("\n");

    if (auto agent = userAgentToAdd(req); !agent.empty())
        lines.append("User-Agent:").append(agent).append("\n");

    return lines;
}

std::map<std::string, std::string> parseWebHttpHeaderBlock(const std::string& block)
{
    auto headers = std::map<std::string, std::string>();
    auto start = std::size_t {0};

    while (start < block.size())
    {
        auto end = block.find('\n', start);

        if (end == std::string::npos)
            end = block.size();

        addHeaderLine(std::string_view(block).substr(start, end - start), headers);
        start = end + 1;
    }

    return headers;
}

// Under a Content-Encoding the declared length is the encoded one, and the
// body arrives decoded, so it says nothing about the bytes to come.
std::int64_t webHttpBodyLength(const Response& response)
{
    auto length = findHeaderIgnoringCase(response.headers, "Content-Length");

    if (length.empty()
        || !findHeaderIgnoringCase(response.headers, "Content-Encoding").empty())
        return -1;

    return Strings::parseOr<std::int64_t>(Strings::trim(length), -1);
}

// A limit of zero redirects is checked here on the redirect itself, so a
// request that follows none is "manual" as well as one that will not follow.
bool refusesEveryWebHttpRedirect(const Request& req)
{
    return req.followRedirects && req.maxRedirects == 0;
}

bool isWebHttpRedirectManual(const Request& req)
{
    return !req.followRedirects || refusesEveryWebHttpRedirect(req);
}

std::string webHttpRedirectMode(const Request& req)
{
    return isWebHttpRedirectManual(req) ? "manual" : "follow";
}

std::string webHttpCacheMode(const Request& req)
{
    return req.cachePolicy == CachePolicy::UseLocalCache ? "default" : "no-store";
}

// What a redirect fetch was told not to follow means: a browser answers with
// an opaque redirect, which carries no status and no headers; node answers
// with the 3xx itself.
std::string webHttpRedirectRefusal(const WebHttpTransfer& transfer,
                                   const std::string& type)
{
    const auto& req = transfer.request;
    auto opaque = type == "opaqueredirect";

    if (!isWebHttpRedirectManual(req))
        return {};

    if (refusesEveryWebHttpRedirect(req))
        return opaque || isRedirectStatus(transfer.response.statusCode)
                   ? tooManyRedirectsError(0)
                   : std::string {};

    if (!opaque)
        return {};

    return "The server redirected, and a browser exposes neither the status nor "
           "the location of a redirect it is told not to follow";
}

std::string openWebHttpDestination(WebHttpTransfer& transfer)
{
    const auto& response = transfer.response;
    auto write =
        downloadWriteFor(response.statusCode, response.headers, transfer.resumeFrom);
    auto alreadyOnDisk = write == DownloadWrite::Append ? transfer.resumeFrom : 0;

    transfer.file.reset(std::fopen(transfer.filePath.c_str(),
                                   write == DownloadWrite::Append ? "ab" : "wb"));

    if (!transfer.file)
        return "Failed to open destination file";

    transfer.received = alreadyOnDisk;

    if (auto* progress = transfer.request.progress)
    {
        auto length = webHttpBodyLength(response);
        progress->totalBytes.store(length >= 0 ? alreadyOnDisk + length : -1);
    }

    return {};
}

bool keepsTheFileAsItIs(const WebHttpTransfer& transfer)
{
    const auto& response = transfer.response;
    return !transfer.filePath.empty()
           && downloadWriteFor(
                  response.statusCode, response.headers, transfer.resumeFrom)
                  == DownloadWrite::Keep;
}

std::string acceptWebHttpHead(WebHttpTransfer& transfer, const std::string& type)
{
    if (auto refusal = webHttpRedirectRefusal(transfer, type); !refusal.empty())
        return refusal;

    transfer.limit.maxBytes = transfer.request.maxResponseSize;

    if (keepsTheFileAsItIs(transfer))
        return {};

    if (transfer.limit.exceeds(webHttpBodyLength(transfer.response)))
        return responseTooLargeError(transfer.limit.maxBytes);

    if (transfer.filePath.empty())
        return {};

    return openWebHttpDestination(transfer);
}

std::string acceptWebHttpChunk(WebHttpTransfer& transfer, std::string_view chunk)
{
    if (!transfer.limit.add((std::int64_t) chunk.size()))
        return responseTooLargeError(transfer.limit.maxBytes);

    if (!transfer.file)
    {
        transfer.response.content.append(chunk);
        return {};
    }

    if (std::fwrite(chunk.data(), 1, chunk.size(), transfer.file.get())
        != chunk.size())
        return "Failed to write file: " + transfer.filePath;

    transfer.received += (std::int64_t) chunk.size();

    if (auto* progress = transfer.request.progress)
        progress->bytesReceived.store(transfer.received);

    return {};
}

void startWebHttpTransfer(const Request& req,
                          const std::string& filePath,
                          std::int64_t resumeFrom,
                          WebHttpDone done)
{
    auto& transfers = webHttpTransfers();
    auto id = transfers.nextId++;

    auto* transfer = transfers.live[id].create();
    transfer->request = req;
    transfer->done = std::move(done);
    transfer->filePath = filePath;
    transfer->resumeFrom = resumeFrom;

    if (!transfers.checker)
        transfers.checker.emplace(checkWebHttpTransfers, webHttpCheckInterval);

    eacp_web_http_start(id,
                        req.url.c_str(),
                        req.type.c_str(),
                        webHttpHeaderLines(req).c_str(),
                        req.body.data(),
                        (int) req.body.size(),
                        req.bodyFilePath.c_str(),
                        webHttpRedirectMode(req).c_str(),
                        webHttpCacheMode(req).c_str());
}

Response waitForWebHttp(const std::function<void(WebHttpDone)>& start)
{
    if (!webHttpCanBlock())
        return webHttpFailure(
            "A blocking HTTP request cannot run on the web unless the executable "
            "links eacp-core-nested-pump: use asyncRequest or asyncDownload");

    auto result = std::optional<Response> {};
    start([&result](Response response) { result = std::move(response); });

    while (!result)
        Threads::runEventLoopFor(webHttpCheckInterval);

    return std::move(*result);
}
} // namespace
} // namespace eacp::HTTP

extern "C"
{
    EMSCRIPTEN_KEEPALIVE void
        eacp_web_http_head(int id, int status, const char* headers, const char* type)
    {
        using namespace eacp::HTTP;
        auto* transfer = findWebHttpTransfer(id);

        if (transfer == nullptr)
            return;

        transfer->headReceived = true;
        transfer->lastActivity = WebHttpClock::now();
        transfer->response.statusCode = status;
        transfer->response.headers = parseWebHttpHeaderBlock(headers);

        if (auto error = acceptWebHttpHead(*transfer, type); !error.empty())
            failWebHttp(id, error);
        else if (keepsTheFileAsItIs(*transfer))
        {
            eacp_web_http_abort(id);
            finishWebHttp(id, transfer->response);
        }
    }

    EMSCRIPTEN_KEEPALIVE void
        eacp_web_http_chunk(int id, const char* data, int length)
    {
        using namespace eacp::HTTP;
        auto* transfer = findWebHttpTransfer(id);

        if (transfer == nullptr)
            return;

        transfer->lastActivity = WebHttpClock::now();
        auto chunk = std::string_view(data, (std::size_t) length);

        if (auto error = acceptWebHttpChunk(*transfer, chunk); !error.empty())
            failWebHttp(id, error);
    }

    EMSCRIPTEN_KEEPALIVE void eacp_web_http_done(int id, const char* error)
    {
        using namespace eacp::HTTP;
        auto* transfer = findWebHttpTransfer(id);

        if (transfer == nullptr)
            return;

        if (error != nullptr)
            return finishWebHttp(id, webHttpFailure(error));

        if (transfer->file && std::fflush(transfer->file.get()) != 0)
            return finishWebHttp(
                id, webHttpFailure("Failed to write file: " + transfer->filePath));

        transfer->file.reset();
        finishWebHttp(id, transfer->response);
    }
}

namespace eacp::HTTP
{
void startWebRequest(const Request& req, WebHttpDone done)
{
    startWebHttpTransfer(req, {}, 0, std::move(done));
}

void startWebDownload(const Request& req,
                      const std::string& filePath,
                      WebHttpDone done)
{
    auto finish =
        [progress = req.progress, done = std::move(done)](Response response)
    {
        if (progress != nullptr)
            progress->done.store(true);

        done(std::move(response));
    };

    auto offset = bytesToResumeFrom(req, filePath);

    if (offset <= 0)
        return startWebHttpTransfer(req, filePath, 0, finish);

    auto settle = [req, filePath, offset, finish](Response response)
    {
        auto settled =
            settleResumedDownload(req, filePath, offset, std::move(response));

        if (settled)
            finish(std::move(*settled));
        else
            startWebHttpTransfer(req, filePath, 0, finish);
    };

    startWebHttpTransfer(withRangeFrom(req, offset), filePath, offset, settle);
}

bool webHttpCanBlock()
{
    return eacpWebCanSuspend();
}

Response httpRequest(const Request& req)
{
    return waitForWebHttp([&req](WebHttpDone done)
                          { startWebRequest(req, std::move(done)); });
}

Response downloadFileFrom(const Request& req,
                          const std::string& filePath,
                          std::int64_t resumeFrom)
{
    return waitForWebHttp(
        [&](WebHttpDone done)
        { startWebHttpTransfer(req, filePath, resumeFrom, std::move(done)); });
}

// One stream: chunk threads are what the split is for.
Response downloadFileParallel(const Request& req, const std::string& filePath)
{
    return downloadFile(req, filePath);
}
} // namespace eacp::HTTP
