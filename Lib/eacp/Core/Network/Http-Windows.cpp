#include "Http.h"
#include <windows.h>
#include <winhttp.h>
#include <stdexcept>
#include <vector>

namespace eacp::HTTP
{

struct WinHttpHandle
{
    HINTERNET handle = nullptr;

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h)
        : handle(h)
    {
    }

    ~WinHttpHandle()
    {
        if (handle)
            WinHttpCloseHandle(handle);
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept
        : handle(other.handle)
    {
        other.handle = nullptr;
    }

    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (handle)
                WinHttpCloseHandle(handle);
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    operator HINTERNET() const { return handle; }
    explicit operator bool() const { return handle != nullptr; }
};

std::wstring toWideString(const std::string& str)
{
    if (str.empty())
        return {};

    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int) str.size(), nullptr, 0);
    if (size <= 0)
        return {};

    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int) str.size(), result.data(), size);
    return result;
}

std::string getLastErrorMessage()
{
    DWORD error = GetLastError();
    if (error == 0)
        return "Unknown error";

    LPWSTR buffer = nullptr;
    DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                                GetModuleHandleW(L"winhttp.dll"),
                                error,
                                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                (LPWSTR) &buffer,
                                0,
                                nullptr);

    if (size == 0)
    {
        size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                  FORMAT_MESSAGE_IGNORE_INSERTS,
                              nullptr,
                              error,
                              MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                              (LPWSTR) &buffer,
                              0,
                              nullptr);
    }

    std::string result;
    if (size > 0 && buffer)
    {
        int utf8Size =
            WideCharToMultiByte(CP_UTF8, 0, buffer, (int) size, nullptr, 0, nullptr, nullptr);
        if (utf8Size > 0)
        {
            result.resize(utf8Size);
            WideCharToMultiByte(
                CP_UTF8, 0, buffer, (int) size, result.data(), utf8Size, nullptr, nullptr);
        }
        LocalFree(buffer);
    }

    if (result.empty())
        result = "Error code: " + std::to_string(error);

    return result;
}

Response httpRequestInternal(const Request& req)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    auto wideUrl = toWideString(req.url);
    if (wideUrl.empty())
        throw std::runtime_error("URL contains invalid UTF-8 characters");

    URL_COMPONENTS urlComponents = {};
    urlComponents.dwStructSize = sizeof(urlComponents);

    wchar_t hostName[256] = {};
    wchar_t urlPath[2048] = {};

    urlComponents.lpszHostName = hostName;
    urlComponents.dwHostNameLength = sizeof(hostName) / sizeof(wchar_t);
    urlComponents.lpszUrlPath = urlPath;
    urlComponents.dwUrlPathLength = sizeof(urlPath) / sizeof(wchar_t);

    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &urlComponents))
        throw std::runtime_error("Malformed URL format");

    bool useSSL = (urlComponents.nScheme == INTERNET_SCHEME_HTTPS);

    WinHttpHandle session(
        WinHttpOpen(L"eacp-http-client/1.0",
                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS,
                    0));

    if (!session)
        throw std::runtime_error("Failed to create HTTP session: " + getLastErrorMessage());

    INTERNET_PORT port = urlComponents.nPort;
    if (port == 0)
        port = useSSL ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    WinHttpHandle connection(WinHttpConnect(session, hostName, port, 0));

    if (!connection)
        throw std::runtime_error("Failed to connect: " + getLastErrorMessage());

    auto wideMethod = toWideString(req.type);
    DWORD flags = useSSL ? WINHTTP_FLAG_SECURE : 0;

    WinHttpHandle request(WinHttpOpenRequest(connection,
                                             wideMethod.c_str(),
                                             urlPath,
                                             nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             flags));

    if (!request)
        throw std::runtime_error("Failed to create request: " + getLastErrorMessage());

    for (const auto& pair : req.headers)
    {
        auto headerLine = toWideString(pair.first + ": " + pair.second);
        if (!WinHttpAddRequestHeaders(
                request, headerLine.c_str(), (DWORD) -1, WINHTTP_ADDREQ_FLAG_ADD))
        {
            throw std::runtime_error("Failed to add header: " + getLastErrorMessage());
        }
    }

    LPVOID bodyData = WINHTTP_NO_REQUEST_DATA;
    DWORD bodyLength = 0;

    if (!req.body.empty())
    {
        bodyData = (LPVOID) req.body.data();
        bodyLength = (DWORD) req.body.size();
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyData, bodyLength,
                            bodyLength, 0))
    {
        throw std::runtime_error("Failed to send request: " + getLastErrorMessage());
    }

    if (!WinHttpReceiveResponse(request, nullptr))
        throw std::runtime_error("Failed to receive response: " + getLastErrorMessage());

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode,
                             &statusCodeSize,
                             WINHTTP_NO_HEADER_INDEX))
    {
        throw std::runtime_error("Failed to get status code: " + getLastErrorMessage());
    }

    std::string content;
    DWORD bytesAvailable = 0;

    while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0)
    {
        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;

        if (WinHttpReadData(request, buffer.data(), bytesAvailable, &bytesRead))
        {
            content.append(buffer.data(), bytesRead);
        }
        else
        {
            throw std::runtime_error("Failed to read response data: " + getLastErrorMessage());
        }
    }

    Response response;
    response.statusCode = (int) statusCode;
    response.content = std::move(content);

    return response;
}

Response httpRequest(const Request& req)
{
    Response res;

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

} // namespace eacp::HTTP
