#include "Http.h"

#include <curl/curl.h>

#include <cstdio>
#include <stdexcept>
#include <string>

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

size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp)
{
    auto total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
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

void applyCommonOptions(CURL* curl, const Request& req, CurlSlist& headers)
{
    if (req.url.empty())
        throw std::invalid_argument("URL cannot be empty");

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.type.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (!req.body.empty())
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) req.body.size());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
    }

    for (const auto& [k, v]: req.headers)
    {
        auto line = k + ": " + v;
        headers.list = curl_slist_append(headers.list, line.c_str());
    }
    if (headers.list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.list);
}

Response httpRequestInternal(const Request& req)
{
    initCurlOnce();

    auto curl = CurlEasy();
    if (!curl.handle)
        throw std::runtime_error("Failed to initialise curl");

    auto headers = CurlSlist();
    applyCommonOptions(curl.handle, req, headers);

    auto body = std::string();
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &body);

    auto rc = curl_easy_perform(curl.handle);
    if (rc != CURLE_OK)
        throw std::runtime_error(curl_easy_strerror(rc));

    auto response = Response();
    long status = 0;
    curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status);
    response.statusCode = (int) status;
    response.content = std::move(body);
    return response;
}

Response downloadFileInternal(const Request& req, const std::string& filePath)
{
    initCurlOnce();

    auto curl = CurlEasy();
    if (!curl.handle)
        throw std::runtime_error("Failed to initialise curl");

    auto headers = CurlSlist();
    applyCommonOptions(curl.handle, req, headers);

    auto* file = std::fopen(filePath.c_str(), "wb");
    if (!file)
        throw std::runtime_error("Failed to open destination file");

    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, file);

    auto rc = curl_easy_perform(curl.handle);
    std::fclose(file);

    if (rc != CURLE_OK)
        throw std::runtime_error(curl_easy_strerror(rc));

    auto response = Response();
    long status = 0;
    curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status);
    response.statusCode = (int) status;
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
        return downloadFileInternal(req, filePath);
    }
    catch (const std::exception& e)
    {
        res.error = e.what();
        res.statusCode = 0;
    }
    return res;
}

} // namespace eacp::HTTP
