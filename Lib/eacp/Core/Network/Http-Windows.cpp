#include "Http.h"
#include <windows.h>
#include <winhttp.h>
#include <stdexcept>
#include <vector>

namespace eacp::HTTP
{

struct WinHttpHandle
{
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

    operator HINTERNET() const { return handle; }
    explicit operator bool() const { return handle != nullptr; }

    HINTERNET handle = nullptr;
};

struct ParsedUrl
{
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool isHttps = false;
};

std::wstring toWideString(const std::string& str)
{
    if (str.empty())
        return {};

    auto size =
        MultiByteToWideChar(CP_UTF8, 0, str.data(), (int) str.size(), nullptr, 0);
    if (size <= 0)
        return {};

    auto result = std::wstring(size, 0);
    MultiByteToWideChar(
        CP_UTF8, 0, str.data(), (int) str.size(), result.data(), size);
    return result;
}

std::string wideToString(LPCWSTR wideStr, int length)
{
    auto utf8Size = WideCharToMultiByte(
        CP_UTF8, 0, wideStr, length, nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0)
        return {};

    auto result = std::string(utf8Size, 0);
    WideCharToMultiByte(
        CP_UTF8, 0, wideStr, length, result.data(), utf8Size, nullptr, nullptr);
    return result;
}

std::string getLastErrorMessage()
{
    auto error = GetLastError();
    if (error == 0)
        return "Unknown error";

    LPWSTR buffer = nullptr;
    auto size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
        GetModuleHandleW(L"winhttp.dll"),
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR) &buffer,
        0,
        nullptr);

    if (size == 0)
    {
        size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
                                  | FORMAT_MESSAGE_FROM_SYSTEM
                                  | FORMAT_MESSAGE_IGNORE_INSERTS,
                              nullptr,
                              error,
                              MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                              (LPWSTR) &buffer,
                              0,
                              nullptr);
    }

    auto result = std::string();
    if (size > 0 && buffer)
    {
        result = wideToString(buffer, (int) size);
        LocalFree(buffer);
    }

    if (result.empty())
        result = "Error code: " + std::to_string(error);

    return result;
}

ParsedUrl parseUrl(const std::wstring& wideUrl)
{
    auto components = URL_COMPONENTS {};
    components.dwStructSize = sizeof(components);

    auto hostBuffer = std::vector<wchar_t>(256);
    auto pathBuffer = std::vector<wchar_t>(2048);

    components.lpszHostName = hostBuffer.data();
    components.dwHostNameLength = (DWORD) hostBuffer.size();
    components.lpszUrlPath = pathBuffer.data();
    components.dwUrlPathLength = (DWORD) pathBuffer.size();

    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &components))
        throw std::runtime_error("Malformed URL format");

    auto parsed = ParsedUrl {};
    parsed.host = hostBuffer.data();
    parsed.path = pathBuffer.data();
    parsed.port = components.nPort;
    parsed.isHttps = (components.nScheme == INTERNET_SCHEME_HTTPS);
    return parsed;
}

INTERNET_PORT getEffectivePort(const ParsedUrl& url)
{
    if (url.port != 0)
        return url.port;

    if (url.isHttps)
        return INTERNET_DEFAULT_HTTPS_PORT;

    return INTERNET_DEFAULT_HTTP_PORT;
}

DWORD getSecurityFlags(bool isHttps)
{
    if (isHttps)
        return WINHTTP_FLAG_SECURE;

    return 0;
}

WinHttpHandle createSession()
{
    auto session = WinHttpHandle(WinHttpOpen(L"eacp-http-client/1.0",
                                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                             WINHTTP_NO_PROXY_NAME,
                                             WINHTTP_NO_PROXY_BYPASS,
                                             0));
    if (!session)
        throw std::runtime_error("Failed to create HTTP session: "
                                 + getLastErrorMessage());

    return session;
}

WinHttpHandle createConnection(HINTERNET session, const ParsedUrl& url)
{
    auto port = getEffectivePort(url);
    auto connection =
        WinHttpHandle(WinHttpConnect(session, url.host.c_str(), port, 0));

    if (!connection)
        throw std::runtime_error("Failed to connect: " + getLastErrorMessage());

    return connection;
}

WinHttpHandle createRequest(HINTERNET connection,
                            const std::wstring& method,
                            const ParsedUrl& url)
{
    auto flags = getSecurityFlags(url.isHttps);
    auto request = WinHttpHandle(WinHttpOpenRequest(connection,
                                                    method.c_str(),
                                                    url.path.c_str(),
                                                    nullptr,
                                                    WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    flags));
    if (!request)
        throw std::runtime_error("Failed to create request: "
                                 + getLastErrorMessage());

    return request;
}

void addHeaders(HINTERNET request, const std::map<std::string, std::string>& headers)
{
    for (const auto& pair: headers)
    {
        auto headerLine = toWideString(pair.first + ": " + pair.second);
        if (!WinHttpAddRequestHeaders(
                request, headerLine.c_str(), (DWORD) -1, WINHTTP_ADDREQ_FLAG_ADD))
        {
            throw std::runtime_error("Failed to add header: "
                                     + getLastErrorMessage());
        }
    }
}

void sendRequest(HINTERNET request, const std::string& body)
{
    auto bodyData = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID) body.data();
    auto bodyLength = (DWORD) body.size();

    if (!WinHttpSendRequest(request,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            bodyData,
                            bodyLength,
                            bodyLength,
                            0))
    {
        throw std::runtime_error("Failed to send request: " + getLastErrorMessage());
    }

    if (!WinHttpReceiveResponse(request, nullptr))
        throw std::runtime_error("Failed to receive response: "
                                 + getLastErrorMessage());
}

int getStatusCode(HINTERNET request)
{
    auto statusCode = DWORD {0};
    auto statusCodeSize = DWORD {sizeof(statusCode)};

    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode,
                             &statusCodeSize,
                             WINHTTP_NO_HEADER_INDEX))
    {
        throw std::runtime_error("Failed to get status code: "
                                 + getLastErrorMessage());
    }

    return (int) statusCode;
}

std::string readResponseContent(HINTERNET request)
{
    auto content = std::string();
    auto bytesAvailable = DWORD {0};

    while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0)
    {
        auto buffer = std::vector<char>(bytesAvailable);
        auto bytesRead = DWORD {0};

        if (WinHttpReadData(request, buffer.data(), bytesAvailable, &bytesRead))
        {
            content.append(buffer.data(), bytesRead);
        }
        else
        {
            throw std::runtime_error("Failed to read response data: "
                                     + getLastErrorMessage());
        }
    }

    return content;
}

Response httpRequestInternal(const Request& req)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    auto wideUrl = toWideString(req.url);
    if (wideUrl.empty())
        throw std::runtime_error("URL contains invalid UTF-8 characters");

    auto url = parseUrl(wideUrl);
    auto session = createSession();
    auto connection = createConnection(session, url);
    auto wideMethod = toWideString(req.type);
    auto request = createRequest(connection, wideMethod, url);

    addHeaders(request, req.headers);
    sendRequest(request, req.body);

    auto response = Response();
    response.statusCode = getStatusCode(request);
    response.content = readResponseContent(request);

    return response;
}

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

} // namespace eacp::HTTP
