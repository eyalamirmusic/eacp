#include <eacp/Core/Utils/WinInclude.h>

#include "Http.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include <cstdint>
#include <eacp/Core/Utils/Containers.h>

namespace eacp::HTTP
{

namespace winhttp = winrt::Windows::Web::Http;
namespace streams = winrt::Windows::Storage::Streams;
using Method = winhttp::HttpMethod;

namespace
{

Method toHttpMethod(const std::string& method)
{
    if (method == "GET")
        return Method::Get();
    if (method == "POST")
        return Method::Post();
    if (method == "PUT")
        return Method::Put();
    if (method == "DELETE")
        return Method::Delete();
    if (method == "PATCH")
        return Method::Patch();
    if (method == "HEAD")
        return Method::Head();
    if (method == "OPTIONS")
        return Method::Options();

    return Method(winrt::to_hstring(method));
}

void ensureMultiThreadedApartment()
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (const winrt::hresult_error&)
    {
    }
}

void appendRequestHeaders(winhttp::HttpRequestMessage& message,
                          const std::map<std::string, std::string>& headers)
{
    for (const auto& [key, value]: headers)
    {
        auto hKey = winrt::to_hstring(key);
        auto hValue = winrt::to_hstring(value);

        if (message.Headers().TryAppendWithoutValidation(hKey, hValue))
            continue;

        if (message.Content())
            message.Content().Headers().TryAppendWithoutValidation(hKey, hValue);
    }
}

void appendContentHeaders(winhttp::HttpStringContent& content,
                          const std::map<std::string, std::string>& headers)
{
    for (const auto& [key, value]: headers)
    {
        auto hKey = winrt::to_hstring(key);
        auto hValue = winrt::to_hstring(value);
        content.Headers().TryAppendWithoutValidation(hKey, hValue);
    }
}

void attachBody(winhttp::HttpRequestMessage& message, const Request& req)
{
    if (req.body.empty())
        return;

    auto content = winhttp::HttpStringContent(winrt::to_hstring(req.body),
                                              streams::UnicodeEncoding::Utf8);

    appendContentHeaders(content, req.headers);
    message.Content(content);
}

winhttp::HttpRequestMessage buildRequestMessage(const Request& req)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    auto uri = winrt::Windows::Foundation::Uri(winrt::to_hstring(req.url));
    auto message = winhttp::HttpRequestMessage(toHttpMethod(req.type), uri);

    appendRequestHeaders(message, req.headers);
    attachBody(message, req);

    return message;
}

void copyResponseHeaders(const winhttp::HttpResponseMessage& source,
                         Response& response)
{
    for (auto&& [name, value]: source.Headers())
        response.headers[winrt::to_string(name)] = winrt::to_string(value);

    if (source.Content())
    {
        for (auto&& [name, value]: source.Content().Headers())
            response.headers[winrt::to_string(name)] = winrt::to_string(value);
    }
}

Response httpRequestInternal(const Request& req)
{
    auto message = buildRequestMessage(req);
    auto client = winhttp::HttpClient();
    auto responseMessage = client.SendRequestAsync(message).get();

    auto response = Response();
    response.statusCode = static_cast<int>(responseMessage.StatusCode());
    copyResponseHeaders(responseMessage, response);

    auto rawRes = responseMessage.Content().ReadAsStringAsync().get();
    response.content = winrt::to_string(rawRes);

    return response;
}

std::int64_t readContentLength(const winhttp::HttpResponseMessage& responseMessage)
{
    auto contentLength = responseMessage.Content().Headers().ContentLength();

    if (!contentLength)
        return -1;

    return static_cast<std::int64_t>(contentLength.GetUInt64());
}

HANDLE openDestinationFileForWrite(const std::string& filePath)
{
    auto wideFilePath = winrt::to_hstring(filePath);
    auto handle = CreateFileW(wideFilePath.c_str(),
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

void writeChunkToFile(HANDLE handle,
                      const streams::IBuffer& chunk,
                      const std::string& filePath)
{
    auto len = chunk.Length();
    auto reader = streams::DataReader::FromBuffer(chunk);
    auto bytes = Vector<uint8_t>((int) len);
    reader.ReadBytes(bytes.getVector());

    DWORD written = 0;
    if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(len), &written, nullptr))
        throw std::runtime_error("Failed to write file: " + filePath);
}

void streamResponseToFile(const winhttp::HttpResponseMessage& responseMessage,
                          HANDLE handle,
                          const Request& req,
                          const std::string& filePath)
{
    auto inputStream = responseMessage.Content().ReadAsInputStreamAsync().get();
    auto buffer = streams::Buffer(64 * 1024);
    auto received = std::int64_t(0);

    while (true)
    {
        if (req.progress && req.progress->cancel.load())
            throw std::runtime_error("Download cancelled");

        auto chunk = inputStream
                         .ReadAsync(buffer,
                                    buffer.Capacity(),
                                    streams::InputStreamOptions::Partial)
                         .get();

        auto len = chunk.Length();
        if (len == 0)
            break;

        writeChunkToFile(handle, chunk, filePath);
        received += static_cast<std::int64_t>(len);

        if (req.progress)
            req.progress->bytesReceived.store(received);
    }
}

Response downloadFileInternal(const Request& req, const std::string& filePath)
{
    auto message = buildRequestMessage(req);
    auto client = winhttp::HttpClient();
    auto responseMessage =
        client
            .SendRequestAsync(message,
                              winhttp::HttpCompletionOption::ResponseHeadersRead)
            .get();

    auto response = Response();
    response.statusCode = static_cast<int>(responseMessage.StatusCode());
    copyResponseHeaders(responseMessage, response);

    if (req.progress)
        req.progress->totalBytes.store(readContentLength(responseMessage));

    auto handle = openDestinationFileForWrite(filePath);

    try
    {
        streamResponseToFile(responseMessage, handle, req, filePath);
    }
    catch (...)
    {
        CloseHandle(handle);
        throw;
    }

    CloseHandle(handle);
    return response;
}

void captureExceptionAsError(Response& res, const std::exception& e)
{
    res.error = e.what();
    res.statusCode = 0;
}

void captureWinrtErrorAsError(Response& res, const winrt::hresult_error& e)
{
    res.error = winrt::to_string(e.message());
    res.statusCode = 0;
}

} // namespace

Response httpRequest(const Request& req)
{
    ensureMultiThreadedApartment();

    auto res = Response();

    try
    {
        return httpRequestInternal(req);
    }
    catch (const winrt::hresult_error& e)
    {
        captureWinrtErrorAsError(res, e);
    }
    catch (const std::exception& e)
    {
        captureExceptionAsError(res, e);
    }

    return res;
}

Response downloadFile(const Request& req, const std::string& filePath)
{
    ensureMultiThreadedApartment();

    auto res = Response();

    try
    {
        res = downloadFileInternal(req, filePath);
    }
    catch (const winrt::hresult_error& e)
    {
        captureWinrtErrorAsError(res, e);
    }
    catch (const std::exception& e)
    {
        captureExceptionAsError(res, e);
    }

    if (req.progress)
        req.progress->done.store(true);

    return res;
}

} // namespace eacp::HTTP
