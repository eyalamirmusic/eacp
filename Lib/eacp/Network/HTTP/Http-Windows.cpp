#include <eacp/Core/Utils/WinInclude.h>

#include "Http.h"

#include <winhttp.h>

// WinHTTP (classic Win32) rather than Windows.Web.Http (WinRT): no apartment
// requirement, no cppwinrt include cost, and bodies travel as raw bytes both
// ways — the WinRT backend routed them through UTF-8 *string* content, which
// corrupted binary payloads (e.g. multipart file uploads).
namespace eacp::HTTP
{

namespace
{

std::wstring toWide(const std::string& utf8)
{
    if (utf8.empty())
        return {};

    auto length = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    auto wide = std::wstring(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

std::string fromWide(const std::wstring& wide)
{
    if (wide.empty())
        return {};

    auto length = WideCharToMultiByte(CP_UTF8,
                                      0,
                                      wide.data(),
                                      static_cast<int>(wide.size()),
                                      nullptr,
                                      0,
                                      nullptr,
                                      nullptr);
    auto utf8 = std::string(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        wide.data(),
                        static_cast<int>(wide.size()),
                        utf8.data(),
                        length,
                        nullptr,
                        nullptr);
    return utf8;
}

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

    auto message = Strings::trim(fromWide(text));
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

struct CrackedUrl
{
    std::wstring host;
    std::wstring pathWithQuery;
    INTERNET_PORT port = 0;
    bool secure = false;
};

CrackedUrl crackUrl(const std::string& url)
{
    auto wide = toWide(url);

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

struct OpenedRequest
{
    Handle connection;
    Handle request;
};

OpenedRequest sendRequest(const Request& req)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    if (!session())
        throwLastError("Opening the WinHTTP session");

    auto cracked = crackUrl(req.url);

    auto opened = OpenedRequest {};
    opened.connection =
        Handle(WinHttpConnect(session(), cracked.host.c_str(), cracked.port, 0));

    if (!opened.connection)
        throwLastError("Connecting");

    opened.request =
        Handle(WinHttpOpenRequest(opened.connection.handle,
                                  toWide(req.type).c_str(),
                                  cracked.pathWithQuery.c_str(),
                                  nullptr,
                                  WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  cracked.secure ? WINHTTP_FLAG_SECURE : 0));

    if (!opened.request)
        throwLastError("Opening the request");

    // Responses arrive decompressed, matching NSURLSession and the previous
    // backend. Best-effort: unsupported systems just skip Accept-Encoding.
    auto decompression = DWORD {WINHTTP_DECOMPRESSION_FLAG_ALL};
    WinHttpSetOption(opened.request.handle,
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

    auto headerBlock = toWide(headerLines);

    if (!headerBlock.empty())
        WinHttpAddRequestHeaders(opened.request.handle,
                                 headerBlock.c_str(),
                                 static_cast<DWORD>(headerBlock.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD);

    auto bodySize = static_cast<DWORD>(req.body.size());
    auto* bodyData = req.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                      : const_cast<char*>(req.body.data());

    if (!WinHttpSendRequest(opened.request.handle,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            bodyData,
                            bodySize,
                            bodySize,
                            0))
        throwLastError("Sending the request");

    if (!WinHttpReceiveResponse(opened.request.handle, nullptr))
        throwLastError("Receiving the response");

    return opened;
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

    auto text = fromWide(raw);
    auto start = size_t {0};

    while (start < text.size())
    {
        auto end = text.find("\r\n", start);
        if (end == std::string::npos)
            end = text.size();

        auto line = text.substr(start, end - start);
        auto colon = line.find(':');

        if (colon != std::string::npos)
        {
            auto key = Strings::trim(line.substr(0, colon));
            auto value = Strings::trim(line.substr(colon + 1));
            if (!key.empty())
                response.headers[key] = value;
        }

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
        return std::stoll(fromWide(text));
    }
    catch (...)
    {
        return -1;
    }
}

std::string readBodyToString(HINTERNET request)
{
    auto body = std::string();

    while (true)
    {
        auto available = DWORD {0};
        if (!WinHttpQueryDataAvailable(request, &available))
            throwLastError("Reading the response");

        if (available == 0)
            return body;

        auto offset = body.size();
        body.resize(offset + available);

        auto read = DWORD {0};
        if (!WinHttpReadData(request, body.data() + offset, available, &read))
            throwLastError("Reading the response");

        body.resize(offset + read);

        if (read == 0)
            return body;
    }
}

HANDLE openDestinationFileForWrite(const std::string& filePath)
{
    auto handle = CreateFileW(toWide(filePath).c_str(),
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

void streamBodyToFile(HINTERNET request,
                      HANDLE file,
                      const Request& req,
                      const std::string& filePath)
{
    auto buffer = std::string(64 * 1024, '\0');
    auto received = std::int64_t {0};

    while (true)
    {
        if (req.progress && req.progress->cancel.load())
            throw std::runtime_error("Download cancelled");

        auto available = DWORD {0};
        if (!WinHttpQueryDataAvailable(request, &available))
            throwLastError("Reading the response");

        if (available == 0)
            return;

        auto toRead = std::min(available, static_cast<DWORD>(buffer.size()));
        auto read = DWORD {0};
        if (!WinHttpReadData(request, buffer.data(), toRead, &read))
            throwLastError("Reading the response");

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
    auto opened = sendRequest(req);

    auto response = Response();
    response.statusCode = queryStatusCode(opened.request.handle);
    copyResponseHeaders(opened.request.handle, response);
    response.content = readBodyToString(opened.request.handle);
    return response;
}

Response downloadFileInternal(const Request& req, const std::string& filePath)
{
    auto opened = sendRequest(req);

    auto response = Response();
    response.statusCode = queryStatusCode(opened.request.handle);
    copyResponseHeaders(opened.request.handle, response);

    if (req.progress)
        req.progress->totalBytes.store(queryContentLength(opened.request.handle));

    auto file = openDestinationFileForWrite(filePath);

    try
    {
        streamBodyToFile(opened.request.handle, file, req, filePath);
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
