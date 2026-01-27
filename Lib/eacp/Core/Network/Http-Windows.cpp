#include "Http.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.Streams.h>

namespace eacp::HTTP
{

namespace winhttp = winrt::Windows::Web::Http;

winhttp::HttpMethod getHttpMethod(const std::string& method)
{
    if (method == "GET")
        return winhttp::HttpMethod::Get();
    if (method == "POST")
        return winhttp::HttpMethod::Post();
    if (method == "PUT")
        return winhttp::HttpMethod::Put();
    if (method == "DELETE")
        return winhttp::HttpMethod::Delete();
    if (method == "PATCH")
        return winhttp::HttpMethod::Patch();
    if (method == "HEAD")
        return winhttp::HttpMethod::Head();
    if (method == "OPTIONS")
        return winhttp::HttpMethod::Options();

    return winhttp::HttpMethod(winrt::to_hstring(method));
}

Response httpRequestInternal(const Request& req)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    winhttp::HttpClient client;

    winrt::Windows::Foundation::Uri uri(winrt::to_hstring(req.url));
    winhttp::HttpRequestMessage requestMessage(getHttpMethod(req.type), uri);

    for (const auto& [key, value]: req.headers)
    {
        auto hKey = winrt::to_hstring(key);
        auto hValue = winrt::to_hstring(value);

        if (!requestMessage.Headers().TryAppendWithoutValidation(hKey, hValue))
        {
            if (requestMessage.Content())
            {
                requestMessage.Content().Headers().TryAppendWithoutValidation(hKey,
                                                                              hValue);
            }
        }
    }

    if (!req.body.empty())
    {
        auto content =
            winhttp::HttpStringContent(winrt::to_hstring(req.body),
                                       winrt::Windows::Storage::Streams::
                                           UnicodeEncoding::Utf8);

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
    response.content = winrt::to_string(responseMessage.Content().ReadAsStringAsync().get());

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

} // namespace eacp::HTTP
