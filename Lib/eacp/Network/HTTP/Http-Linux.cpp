#include "Http.h"
#include "HttpProtocol.h"

#include <curl/curl.h>

#include <chrono>
#include <cstdio>

namespace eacp::HTTP
{

namespace
{

void initCurlOnce()
{
    static auto once = []
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return 0;
    }();
    (void) once;
}

// Runs from curl's progress callback, which curl calls often while bytes move
// and about once a second while they do not: reports download progress,
// passes a cancel on, and ends a transfer that has gone quiet for longer than
// the idle limit.
class TransferWatch
{
    using Clock = std::chrono::steady_clock;

public:
    TransferWatch(DownloadProgress* progressToReport, Time::MS idleLimitToUse)
        : progress(progressToReport)
        , idleLimit(idleLimitToUse)
    {
    }

    bool update(curl_off_t total, curl_off_t received, curl_off_t sent)
    {
        reportProgress(total, received);
        return isCancelled() || hasBeenIdleTooLong(received + sent);
    }

    bool needsCallback() const { return progress != nullptr || idleLimit.count > 0; }

    bool canBeCancelled() const { return progress != nullptr; }

    bool isCancelled() const
    {
        return progress != nullptr && progress->cancel.load();
    }

    bool wentIdle() const { return idleExpired; }

    // A resumed download reports nothing until the response says whether its
    // body extends the file, replaces it or - an error page - is not the
    // file at all; from then on the bytes already on disk are counted in.
    void waitForTheResponse() { reporting = false; }

    // A plain request takes the cancel from its progress block but reports
    // nothing into it, as on the other backends.
    void reportNothing() { reporting = false; }

    void startReporting(std::int64_t bytesAlreadyOnDisk)
    {
        offset = bytesAlreadyOnDisk;
        reporting = true;
    }

private:
    void reportProgress(curl_off_t total, curl_off_t received)
    {
        if (progress == nullptr || !reporting)
            return;

        progress->bytesReceived.store(offset + received);
        progress->totalBytes.store(total == 0 ? -1 : offset + total);
    }

    bool hasBeenIdleTooLong(curl_off_t moved)
    {
        if (idleLimit.count <= 0)
            return false;

        auto now = Clock::now();

        if (moved != lastMoved)
        {
            lastMoved = moved;
            lastActivity = now;
            return false;
        }

        auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastActivity);
        idleExpired = quiet.count() >= idleLimit.count;
        return idleExpired;
    }

    DownloadProgress* progress = nullptr;
    Time::MS idleLimit;
    std::int64_t offset = 0;
    bool reporting = true;
    curl_off_t lastMoved = 0;
    Clock::time_point lastActivity = Clock::now();
    bool idleExpired = false;
};

int progressCallback(
    void* userp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t ulnow)
{
    auto* watch = static_cast<TransferWatch*>(userp);
    return watch->update(dltotal, dlnow, ulnow) ? 1 : 0;
}

struct BodyToString
{
    std::string* body = nullptr;
    ResponseSizeLimit limit;
    bool tooLarge = false;
};

// The destination is opened on the first byte of the final response, once
// its status says whether the body replaces the file, extends it, or is not
// wanted at all.
struct BodyToFile
{
    ~BodyToFile() { close(); }

    void close()
    {
        if (file != nullptr)
            std::fclose(file);

        file = nullptr;
    }

    bool decide()
    {
        if (decided)
            return !openFailed;

        decided = true;

        auto status = 0L;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        write = downloadWriteFor((int) status, *headers, resumeFrom);

        if (write == DownloadWrite::Keep)
            return true;

        watch->startReporting(write == DownloadWrite::Append ? resumeFrom : 0);

        file =
            std::fopen(path.c_str(), write == DownloadWrite::Append ? "ab" : "wb");
        openFailed = file == nullptr;
        return !openFailed;
    }

    CURL* curl = nullptr;
    const std::map<std::string, std::string>* headers = nullptr;
    TransferWatch* watch = nullptr;
    std::string path;
    std::int64_t resumeFrom = 0;
    std::FILE* file = nullptr;
    ResponseSizeLimit limit;
    DownloadWrite write = DownloadWrite::Replace;
    bool decided = false;
    bool openFailed = false;
    bool tooLarge = false;
};

size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp)
{
    auto total = size * nmemb;
    auto& sink = *static_cast<BodyToString*>(userp);

    if (!sink.limit.add((int64_t) total))
    {
        sink.tooLarge = true;
        return 0;
    }

    sink.body->append(static_cast<char*>(contents), total);
    return total;
}

size_t writeToFile(void* contents, size_t size, size_t nmemb, void* userp)
{
    auto total = size * nmemb;
    auto& sink = *static_cast<BodyToFile*>(userp);

    if (!sink.decide())
        return 0;

    if (!sink.limit.add((int64_t) total))
    {
        sink.tooLarge = true;
        return 0;
    }

    if (sink.file == nullptr)
        return total;

    return std::fwrite(contents, 1, total, sink.file);
}

struct TransferFailure
{
    bool tooLarge = false;
    bool wentIdle = false;
    bool couldNotOpenFile = false;
};

[[noreturn]] void throwTransferError(CURLcode rc,
                                     const TransferFailure& failure,
                                     const Request& req)
{
    if (failure.tooLarge)
        throw std::runtime_error(responseTooLargeError(req.maxResponseSize));

    if (failure.wentIdle)
        throw std::runtime_error(timedOutError());

    if (failure.couldNotOpenFile)
        throw std::runtime_error("Failed to open destination file");

    if (rc == CURLE_TOO_MANY_REDIRECTS)
        throw std::runtime_error(tooManyRedirectsError(req.maxRedirects));

    throw std::runtime_error(curl_easy_strerror(rc));
}

size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userp)
{
    auto total = size * nitems;
    auto& headers = *static_cast<std::map<std::string, std::string>*>(userp);

    addHeaderLine(std::string_view {buffer, total}, headers);

    return total;
}

struct CurlSlist
{
    ~CurlSlist()
    {
        if (list)
            curl_slist_free_all(list);
    }
    curl_slist* list = nullptr;
};

struct CurlEasy
{
    CurlEasy()
        : handle(curl_easy_init())
    {
    }
    ~CurlEasy()
    {
        if (handle)
            curl_easy_cleanup(handle);
    }
    CURL* handle = nullptr;
};

void applyTimeouts(CURL* curl, const Request& req)
{
    if (req.timeout.count > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long) req.timeout.count);

    auto connect = connectLimit(req);

    if (connect.count > 0)
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long) connect.count);
}

void applyRedirectPolicy(CURL* curl, const Request& req)
{
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.followRedirects ? 1L : 0L);

    if (req.followRedirects && req.maxRedirects >= 0)
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long) req.maxRedirects);
}

void applyCommonOptions(CURL* curl, const Request& req, CurlSlist& headers)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.type.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    applyRedirectPolicy(curl, req);
    applyTimeouts(curl, req);

    if (auto agent = userAgentToAdd(req); !agent.empty())
        curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());

    if (req.type == "HEAD")
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    if (!req.body.empty())
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) req.body.size());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
    }

    for (const auto& [k, v]: req.headers)
    {
        auto line = k;
        line.append(": ").append(v);
        headers.list = curl_slist_append(headers.list, line.c_str());
    }
    if (headers.list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.list);
}

void watchTransfer(CURL* curl, TransferWatch& watch)
{
    if (!watch.needsCallback())
        return;

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &watch);
}

struct CurlMulti
{
    CurlMulti(CURL* easyToRun)
        : handle(curl_multi_init())
        , easy(easyToRun)
    {
        if (!handle)
            throw std::runtime_error("Failed to initialise curl");

        curl_multi_add_handle(handle, easy);
    }

    ~CurlMulti()
    {
        curl_multi_remove_handle(handle, easy);
        curl_multi_cleanup(handle);
    }

    CURLM* handle = nullptr;
    CURL* easy = nullptr;
};

constexpr auto cancelPollMilliseconds = 25;

CURLcode finishedTransferResult(CURLM* multi)
{
    auto queued = 0;

    while (auto* message = curl_multi_info_read(multi, &queued))
    {
        if (message->msg == CURLMSG_DONE)
            return message->data.result;
    }

    return CURLE_OK;
}

// curl only offers the cancel flag to the progress callback, which it calls
// about once a second while nothing moves. A transfer that can be cancelled
// is driven through a multi handle instead, which wakes every few
// milliseconds to look. A transfer that has finished keeps its own result,
// whatever the flag says by then.
CURLcode performWatchingForCancel(CURL* easy, const TransferWatch& watch)
{
    if (!watch.canBeCancelled())
        return curl_easy_perform(easy);

    auto multi = CurlMulti(easy);

    while (true)
    {
        auto running = 0;
        auto rc = curl_multi_perform(multi.handle, &running);

        if (rc != CURLM_OK)
            throw std::runtime_error(curl_multi_strerror(rc));

        if (running == 0)
            return finishedTransferResult(multi.handle);

        if (watch.isCancelled())
            return CURLE_ABORTED_BY_CALLBACK;

        curl_multi_poll(multi.handle, nullptr, 0, cancelPollMilliseconds, nullptr);
    }
}

int responseCode(CURL* curl)
{
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    return (int) status;
}

Response httpRequestInternal(const Request& req)
{
    initCurlOnce();

    auto curl = CurlEasy();
    if (!curl.handle)
        throw std::runtime_error("Failed to initialise curl");

    auto headers = CurlSlist();
    applyCommonOptions(curl.handle, req, headers);

    auto response = Response();
    auto sink = BodyToString {};
    sink.body = &response.content;
    sink.limit.maxBytes = req.maxResponseSize;
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &sink);

    curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &response.headers);

    auto watch = TransferWatch(req.progress, req.idleTimeout);
    watch.reportNothing();
    watchTransfer(curl.handle, watch);

    auto rc = performWatchingForCancel(curl.handle, watch);
    if (rc != CURLE_OK)
        throwTransferError(rc, {sink.tooLarge, watch.wentIdle(), false}, req);

    response.statusCode = responseCode(curl.handle);
    return response;
}

Response downloadFileInternal(const Request& req,
                              const std::string& filePath,
                              std::int64_t resumeFrom)
{
    initCurlOnce();

    auto curl = CurlEasy();
    if (!curl.handle)
        throw std::runtime_error("Failed to initialise curl");

    auto headers = CurlSlist();
    applyCommonOptions(curl.handle, req, headers);

    auto response = Response();
    curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &response.headers);

    auto watch = TransferWatch(req.progress, req.idleTimeout);
    watchTransfer(curl.handle, watch);

    if (resumeFrom > 0)
        watch.waitForTheResponse();

    auto sink = BodyToFile {};
    sink.curl = curl.handle;
    sink.headers = &response.headers;
    sink.watch = &watch;
    sink.path = filePath;
    sink.resumeFrom = resumeFrom;
    sink.limit.maxBytes = req.maxResponseSize;
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &sink);

    auto rc = performWatchingForCancel(curl.handle, watch);

    if (rc == CURLE_OK && !sink.decide())
        rc = CURLE_WRITE_ERROR;

    sink.close();

    if (rc != CURLE_OK)
        throwTransferError(
            rc, {sink.tooLarge, watch.wentIdle(), sink.openFailed}, req);

    response.statusCode = responseCode(curl.handle);
    return response;
}

} // namespace

Response httpRequest(const Request& req)
{
    auto res = Response();
    try
    {
        return httpRequestInternal(req);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }
    return res;
}

Response downloadFileFrom(const Request& req,
                          const std::string& filePath,
                          std::int64_t resumeFrom)
{
    auto res = Response();
    try
    {
        res = downloadFileInternal(req, filePath, resumeFrom);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }

    return res;
}

} // namespace eacp::HTTP
