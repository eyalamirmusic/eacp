#include "Http.h"

#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

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
        auto response = httpRequestInternal(req);

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

        DWORD written = 0;
        WriteFile(handle,
                  response.content.data(),
                  static_cast<DWORD>(response.content.size()),
                  &written,
                  nullptr);
        CloseHandle(handle);

        response.content.clear();
        return response;
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

} // namespace eacp::HTTP
