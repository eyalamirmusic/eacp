#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Utils/WinInclude.h>

#include "Http.h"
#include "HttpProtocol.h"

#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

// WinHTTP (classic Win32) rather than Windows.Web.Http (WinRT): no apartment
// requirement, no cppwinrt include cost, and bodies travel as raw bytes both
// ways — the WinRT backend routed them through UTF-8 *string* content, which
// corrupted binary payloads (e.g. multipart file uploads).
namespace eacp::HTTP
{

namespace
{

[[noreturn]] void throwLastError(const std::string& what)
{
    auto code = GetLastError();

    // WinHTTP error strings live in winhttp.dll's message table, not the
    // system's, so search both.
    wchar_t text[512] {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE
                       | FORMAT_MESSAGE_IGNORE_INSERTS,
                   GetModuleHandleW(L"winhttp.dll"),
                   code,
                   0,
                   text,
                   static_cast<DWORD>(std::size(text)),
                   nullptr);

    auto message = Strings::trim(Strings::narrow(text));
    if (message.empty())
        message = what + " failed (error " + std::to_string(code) + ")";

    throw std::runtime_error(message);
}

struct Handle
{
    Handle() = default;

    explicit Handle(HINTERNET handleToUse)
        : handle(handleToUse)
    {
    }

    Handle(Handle&& other) noexcept
        : handle(std::exchange(other.handle, nullptr))
    {
    }

    Handle& operator=(Handle&& other) noexcept
    {
        std::swap(handle, other.handle);
        return *this;
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    ~Handle()
    {
        if (handle)
            WinHttpCloseHandle(handle);
    }

    explicit operator bool() const { return handle != nullptr; }

    HINTERNET handle = nullptr;
};

// One process-wide session, never closed: WinHTTP session handles are
// thread-safe and cheap to share. AUTOMATIC_PROXY honours the system proxy
// configuration (as Windows.Web.Http did); the fallback covers systems
// predating it (Win8.0).
HINTERNET session()
{
    static auto handle = []
    {
        auto opened = WinHttpOpen(L"eacp",
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS,
                                  0);
        if (!opened)
            opened = WinHttpOpen(L"eacp",
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS,
                                 0);
        return opened;
    }();

    return handle;
}

// WinHTTP bounds each phase of a request separately - resolving,
// connecting, sending, receiving - so four full-length phases, or a slow
// drip of small reads, can outlive all of them. The read loops check this
// too, which makes the limit cover the whole request the way the curl and
// NSURLSession backends do.
class RequestTimeout
{
public:
    explicit RequestTimeout(const Request& req)
        : limited(req.timeout.count > 0)
        , deadline(req.timeout)
    {
    }

    void throwIfExpired() const
    {
        if (limited && deadline.expired())
            throw std::runtime_error("The request timed out");
    }

private:
    bool limited = false;
    Time::Deadline deadline;
};

void applyTimeouts(HINTERNET request, const Request& req)
{
    if (req.timeout.count <= 0)
        return;

    auto maxMilliseconds = std::numeric_limits<int>::max();
    auto limit = req.timeout.count > maxMilliseconds
                     ? maxMilliseconds
                     : static_cast<int>(req.timeout.count);

    WinHttpSetTimeouts(request, limit, limit, limit, limit);
}

// Those timeouts are recorded but never interrupt a call already in flight:
// against a server that accepts the connection and then goes quiet,
// WinHttpSendRequest stays blocked until the reply finally arrives, and the
// limit is only reported afterwards - far too late to be the wall-clock bound
// the curl and NSURLSession backends give. Closing the request handle is the
// one thing that does cut a blocked call short, so the handle lives here with
// a thread that closes it at the deadline; the call then fails with
// ERROR_WINHTTP_OPERATION_CANCELLED and unwinds as a timeout.
class TimedRequestHandle
{
public:
    ~TimedRequestHandle()
    {
        stopWatchdog();
        closeOnce();
    }

    // Takes ownership of the handle and starts the clock. Called before the
    // first blocking call, so the whole exchange sits inside the window.
    void adopt(HINTERNET requestToOwn, Time::MS timeout)
    {
        handle = requestToOwn;

        if (timeout.count > 0)
            watchdog = std::thread([this, timeout] { watch(timeout); });
    }

    HINTERNET get() const { return handle; }

    void throwIfTimedOut() const
    {
        if (expired.load())
            throw std::runtime_error("The request timed out");
    }

private:
    void watch(Time::MS timeout)
    {
        auto lock = std::unique_lock(mutex);
        auto due = std::chrono::steady_clock::now()
                   + std::chrono::milliseconds(timeout.count);

        if (settled.wait_until(lock, due, [this] { return finished; }))
            return;

        lock.unlock();

        expired.store(true);
        closeOnce();
    }

    void stopWatchdog()
    {
        {
            auto lock = std::scoped_lock(mutex);
            finished = true;
        }

        settled.notify_all();

        if (watchdog.joinable())
            watchdog.join();
    }

    // Whichever of the watchdog and the destructor gets here first does the
    // close, so a cancelled handle is never closed twice.
    void closeOnce()
    {
        if (handle && !closed.exchange(true))
            WinHttpCloseHandle(handle);
    }

    HINTERNET handle = nullptr;
    std::thread watchdog;
    std::mutex mutex;
    std::condition_variable settled;
    std::atomic<bool> expired {false};
    std::atomic<bool> closed {false};
    bool finished = false;
};

struct CrackedUrl
{
    std::wstring host;
    std::wstring pathWithQuery;
    INTERNET_PORT port = 0;
    bool secure = false;
};

CrackedUrl crackUrl(const std::string& url)
{
    auto wide = Strings::widen(url);

    auto parts = URL_COMPONENTS {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
        throwLastError("Parsing the URL");

    auto cracked = CrackedUrl {};
    cracked.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    cracked.pathWithQuery.assign(parts.lpszUrlPath, parts.dwUrlPathLength);

    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0)
        cracked.pathWithQuery.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    if (cracked.pathWithQuery.empty())
        cracked.pathWithQuery = L"/";

    cracked.port = parts.nPort;
    cracked.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return cracked;
}

// The request is declared last so it is destroyed first: the watchdog gets
// joined and the request closed before the connection it hangs off.
struct OpenedRequest
{
    Handle connection;
    TimedRequestHandle request;
};

void sendRequest(const Request& req, OpenedRequest& opened)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    if (!session())
        throwLastError("Opening the WinHTTP session");

    auto cracked = crackUrl(req.url);

    opened.connection =
        Handle(WinHttpConnect(session(), cracked.host.c_str(), cracked.port, 0));

    if (!opened.connection)
        throwLastError("Connecting");

    auto* request = WinHttpOpenRequest(opened.connection.handle,
                                       Strings::widen(req.type).c_str(),
                                       cracked.pathWithQuery.c_str(),
                                       nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       cracked.secure ? WINHTTP_FLAG_SECURE : 0);

    if (!request)
        throwLastError("Opening the request");

    applyTimeouts(request, req);
    opened.request.adopt(request, req.timeout);

    // Responses arrive decompressed, matching NSURLSession and the previous
    // backend. Best-effort: unsupported systems just skip Accept-Encoding.
    auto decompression = DWORD {WINHTTP_DECOMPRESSION_FLAG_ALL};
    WinHttpSetOption(opened.request.get(),
                     WINHTTP_OPTION_DECOMPRESSION,
                     &decompression,
                     sizeof(decompression));

    auto headerLines = std::string();
    for (const auto& [key, value]: req.headers)
    {
        headerLines.append(key);
        headerLines.append(": ");
        headerLines.append(value);
        headerLines.append("\r\n");
    }

    auto headerBlock = Strings::widen(headerLines);

    if (!headerBlock.empty())
        WinHttpAddRequestHeaders(opened.request.get(),
                                 headerBlock.c_str(),
                                 static_cast<DWORD>(headerBlock.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD);

    auto bodySize = static_cast<DWORD>(req.body.size());
    auto* bodyData = req.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                      : const_cast<char*>(req.body.data());

    if (!WinHttpSendRequest(opened.request.get(),
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            bodyData,
                            bodySize,
                            bodySize,
                            0))
    {
        opened.request.throwIfTimedOut();
        throwLastError("Sending the request");
    }

    if (!WinHttpReceiveResponse(opened.request.get(), nullptr))
    {
        opened.request.throwIfTimedOut();
        throwLastError("Receiving the response");
    }
}

int queryStatusCode(HINTERNET request)
{
    auto status = DWORD {0};
    auto size = DWORD {sizeof(status)};
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &size,
                        WINHTTP_NO_HEADER_INDEX);
    return static_cast<int>(status);
}

// Same line handling as the curl backend: one entry per "Key: Value" line,
// keys kept verbatim, values trimmed. The status line has no colon and skips
// itself.
void copyResponseHeaders(HINTERNET request, Response& response)
{
    auto size = DWORD {0};
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER,
                        &size,
                        WINHTTP_NO_HEADER_INDEX);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return;

    auto raw = std::wstring(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_RAW_HEADERS_CRLF,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             raw.data(),
                             &size,
                             WINHTTP_NO_HEADER_INDEX))
        return;

    auto text = Strings::narrow(raw);
    auto start = size_t {0};

    while (start < text.size())
    {
        auto end = text.find("\r\n", start);
        if (end == std::string::npos)
            end = text.size();

        addHeaderLine(std::string_view {text}.substr(start, end - start),
                      response.headers);

        start = end + 2;
    }
}

std::int64_t queryContentLength(HINTERNET request)
{
    wchar_t text[32] {};
    auto size = DWORD {sizeof(text)};

    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_CONTENT_LENGTH,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             text,
                             &size,
                             WINHTTP_NO_HEADER_INDEX))
        return -1;

    try
    {
        return std::stoll(Strings::narrow(text));
    }
    catch (...)
    {
        return -1;
    }
}

std::string readBodyToString(const TimedRequestHandle& request,
                             const RequestTimeout& timeout)
{
    auto body = std::string();

    while (true)
    {
        timeout.throwIfExpired();

        auto available = DWORD {0};
        if (!WinHttpQueryDataAvailable(request.get(), &available))
        {
            request.throwIfTimedOut();
            throwLastError("Reading the response");
        }

        if (available == 0)
            return body;

        auto offset = body.size();
        body.resize(offset + available);

        auto read = DWORD {0};
        if (!WinHttpReadData(request.get(), body.data() + offset, available, &read))
        {
            request.throwIfTimedOut();
            throwLastError("Reading the response");
        }

        body.resize(offset + read);

        if (read == 0)
            return body;
    }
}

HANDLE openDestinationFileForWrite(const std::string& filePath)
{
    auto handle = CreateFileW(Strings::widen(filePath).c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);

    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Failed to create file: " + filePath);

    return handle;
}

void streamBodyToFile(const TimedRequestHandle& request,
                      HANDLE file,
                      const Request& req,
                      const std::string& filePath,
                      const RequestTimeout& timeout)
{
    auto buffer = std::string(64 * 1024, '\0');
    auto received = std::int64_t {0};

    while (true)
    {
        timeout.throwIfExpired();

        if (req.progress && req.progress->cancel.load())
            throw std::runtime_error("Download cancelled");

        auto available = DWORD {0};
        if (!WinHttpQueryDataAvailable(request.get(), &available))
        {
            request.throwIfTimedOut();
            throwLastError("Reading the response");
        }

        if (available == 0)
            return;

        auto toRead = std::min(available, static_cast<DWORD>(buffer.size()));
        auto read = DWORD {0};
        if (!WinHttpReadData(request.get(), buffer.data(), toRead, &read))
        {
            request.throwIfTimedOut();
            throwLastError("Reading the response");
        }

        if (read == 0)
            return;

        auto written = DWORD {0};
        if (!WriteFile(file, buffer.data(), read, &written, nullptr))
            throw std::runtime_error("Failed to write file: " + filePath);

        received += read;
        if (req.progress)
            req.progress->bytesReceived.store(received);
    }
}

Response httpRequestInternal(const Request& req)
{
    auto timeout = RequestTimeout(req);

    auto opened = OpenedRequest {};
    sendRequest(req, opened);

    auto response = Response();
    response.statusCode = queryStatusCode(opened.request.get());
    copyResponseHeaders(opened.request.get(), response);
    response.content = readBodyToString(opened.request, timeout);
    return response;
}

Response downloadFileInternal(const Request& req, const std::string& filePath)
{
    auto timeout = RequestTimeout(req);

    auto opened = OpenedRequest {};
    sendRequest(req, opened);

    auto response = Response();
    response.statusCode = queryStatusCode(opened.request.get());
    copyResponseHeaders(opened.request.get(), response);

    if (req.progress)
        req.progress->totalBytes.store(queryContentLength(opened.request.get()));

    auto file = openDestinationFileForWrite(filePath);

    try
    {
        streamBodyToFile(opened.request, file, req, filePath, timeout);
    }
    catch (...)
    {
        CloseHandle(file);
        throw;
    }

    CloseHandle(file);
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

Response downloadFile(const Request& req, const std::string& filePath)
{
    auto res = Response();

    try
    {
        res = downloadFileInternal(req, filePath);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }

    if (req.progress)
        req.progress->done.store(true);

    return res;
}

} // namespace eacp::HTTP
