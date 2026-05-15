#include <eacp/Core/Utils/WinInclude.h>

#include "Http.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include <cstdint>
#include <vector>

namespace eacp::HTTP
{

namespace winhttp = winrt::Windows::Web::Http;
using Method = winhttp::HttpMethod;

Method getHttpMethod(const std::string& method)
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

Response httpRequestInternal(const Request& req)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    auto client = winhttp::HttpClient();

    auto urlString = winrt::to_hstring(req.url);
    auto uri = winrt::Windows::Foundation::Uri(urlString);

    auto method = getHttpMethod(req.type);
    auto requestMessage = winhttp::HttpRequestMessage(method, uri);

    for (const auto& [key, value]: req.headers)
    {
        auto hKey = winrt::to_hstring(key);
        auto hValue = winrt::to_hstring(value);

        if (!requestMessage.Headers().TryAppendWithoutValidation(hKey, hValue))
        {
            if (requestMessage.Content())
            {
                requestMessage.Content().Headers().TryAppendWithoutValidation(
                    hKey, hValue);
            }
        }
    }

    if (!req.body.empty())
    {
        auto content = winhttp::HttpStringContent(
            winrt::to_hstring(req.body),
            winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8);

        for (const auto& [key, value]: req.headers)
        {
            auto hKey = winrt::to_hstring(key);
            auto hValue = winrt::to_hstring(value);
            content.Headers().TryAppendWithoutValidation(hKey, hValue);
        }

        requestMessage.Content(content);
    }

    auto responseMessage = client.SendRequestAsync(requestMessage).get();

    auto response = Response();
    response.statusCode = static_cast<int>(responseMessage.StatusCode());

    for (auto&& [name, value]: responseMessage.Headers())
        response.headers[winrt::to_string(name)] = winrt::to_string(value);

    if (responseMessage.Content())
    {
        for (auto&& [name, value]: responseMessage.Content().Headers())
            response.headers[winrt::to_string(name)] = winrt::to_string(value);
    }

    auto rawRes = responseMessage.Content().ReadAsStringAsync().get();
    response.content = winrt::to_string(rawRes);

    return response;
}

Response httpRequest(const Request& req)
{
    auto res = Response();

    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (const winrt::hresult_error&)
    {
        // Already initialized, ignore
    }

    try
    {
        return httpRequestInternal(req);
    }
    catch (const winrt::hresult_error& e)
    {
        res.error = winrt::to_string(e.message());
        res.statusCode = 0;
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }

    return res;
}

Response downloadFileInternal(const Request& req, const std::string& filePath)
{
    namespace streams = winrt::Windows::Storage::Streams;

    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    auto client = winhttp::HttpClient();
    auto uri = winrt::Windows::Foundation::Uri(winrt::to_hstring(req.url));
    auto method = getHttpMethod(req.type);
    auto requestMessage = winhttp::HttpRequestMessage(method, uri);

    for (const auto& [key, value]: req.headers)
    {
        auto hKey = winrt::to_hstring(key);
        auto hValue = winrt::to_hstring(value);

        if (!requestMessage.Headers().TryAppendWithoutValidation(hKey, hValue))
        {
            if (requestMessage.Content())
            {
                requestMessage.Content().Headers().TryAppendWithoutValidation(
                    hKey, hValue);
            }
        }
    }

    if (!req.body.empty())
    {
        auto content = winhttp::HttpStringContent(
            winrt::to_hstring(req.body), streams::UnicodeEncoding::Utf8);

        for (const auto& [key, value]: req.headers)
        {
            auto hKey = winrt::to_hstring(key);
            auto hValue = winrt::to_hstring(value);
            content.Headers().TryAppendWithoutValidation(hKey, hValue);
        }

        requestMessage.Content(content);
    }

    auto responseMessage =
        client.SendRequestAsync(requestMessage,
                                winhttp::HttpCompletionOption::ResponseHeadersRead)
            .get();

    auto response = Response();
    response.statusCode = static_cast<int>(responseMessage.StatusCode());

    for (auto&& [name, value]: responseMessage.Headers())
        response.headers[winrt::to_string(name)] = winrt::to_string(value);

    if (responseMessage.Content())
    {
        for (auto&& [name, value]: responseMessage.Content().Headers())
            response.headers[winrt::to_string(name)] = winrt::to_string(value);
    }

    auto totalBytes = std::int64_t(-1);
    auto contentLength = responseMessage.Content().Headers().ContentLength();
    if (contentLength)
        totalBytes = static_cast<std::int64_t>(contentLength.GetUInt64());

    if (req.progress)
        req.progress->totalBytes.store(totalBytes);

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

    try
    {
        auto inputStream =
            responseMessage.Content().ReadAsInputStreamAsync().get();

        auto buffer = streams::Buffer(64 * 1024);
        auto received = std::int64_t(0);

        while (true)
        {
            if (req.progress && req.progress->cancel.load())
                throw std::runtime_error("Download cancelled");

            auto chunk =
                inputStream
                    .ReadAsync(buffer,
                               buffer.Capacity(),
                               streams::InputStreamOptions::Partial)
                    .get();

            auto len = chunk.Length();
            if (len == 0)
                break;

            auto reader = streams::DataReader::FromBuffer(chunk);
            auto bytes = std::vector<uint8_t>(len);
            reader.ReadBytes(bytes);

            DWORD written = 0;
            if (!WriteFile(handle,
                           bytes.data(),
                           static_cast<DWORD>(len),
                           &written,
                           nullptr))
                throw std::runtime_error("Failed to write file: " + filePath);

            received += static_cast<std::int64_t>(len);

            if (req.progress)
                req.progress->bytesReceived.store(received);
        }
    }
    catch (...)
    {
        CloseHandle(handle);
        throw;
    }

    CloseHandle(handle);
    return response;
}

Response downloadFile(const Request& req, const std::string& filePath)
{
    auto res = Response();

    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (const winrt::hresult_error&)
    {
    }

    try
    {
        res = downloadFileInternal(req, filePath);
    }
    catch (const winrt::hresult_error& e)
    {
        res.error = winrt::to_string(e.message());
        res.statusCode = 0;
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
