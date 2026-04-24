#include <eacp/Core/Utils/Logging.h>
#include <eacp/Network/Http.h>

using namespace eacp;

void printResponse(const eacp::HTTP::Response& res)
{
    LOG("Status: " + std::to_string(res.statusCode));
    if (!res.error.empty())
        LOG("Error: " + res.error);
    else
        LOG("Content: " + res.content);

    LOG("");
}

void performAndPrint(const HTTP::Request& req)
{
    auto res = req.perform();
    printResponse(res);
}

void getAndPrint(const std::string& url)
{
    auto res = HTTP::httpRequest(url);
    printResponse(res);
}

int main()
{
    getAndPrint("https://jsonplaceholder.typicode.com/posts/1");
    getAndPrint("https://jsonplaceholder.typicode.com/posts?userId=1");

    {
        auto req = HTTP::Request("https://httpbin.org/get");
        req.headers["X-Custom-Header"] = "CustomValue";
        req.headers["Accept"] = "application/json";
        performAndPrint(req);
    }

    {
        auto jsonBody = R"({
            "title": "My New Post",
            "body": "This is the content of my post",
            "userId": 1
        })";
        auto req = HTTP::Request::post("https://jsonplaceholder.typicode.com/posts",
                                       jsonBody);
        req.headers["Content-Type"] = "application/json";
        performAndPrint(req);
    }

    {
        auto formData = "username=testuser&password=secret123";
        auto req = HTTP::Request::post("https://httpbin.org/post", formData);
        req.headers["Content-Type"] = "application/x-www-form-urlencoded";
        performAndPrint(req);
    }

    // 6. GET user agent info
    {
        auto req = HTTP::Request("https://httpbin.org/user-agent");
        req.headers["User-Agent"] = "eacp-http-client/1.0";
        performAndPrint(req);
    }

    getAndPrint("https://httpbin.org/ip");

    // 8. GET with authentication header (Bearer token simulation)
    {
        auto req = HTTP::Request("https://httpbin.org/bearer");
        req.headers["Authorization"] = "Bearer my-fake-jwt-token-12345";
        performAndPrint(req);
    }

    // 9. Download a file to disk
    {
        auto req = HTTP::Request("https://jsonplaceholder.typicode.com/posts/1");
        auto res = req.downloadTo("/tmp/eacp-download-test.json");

        LOG("Download status: " + std::to_string(res.statusCode));

        if (!res.error.empty())
            LOG("Download error: " + res.error);
        else
            LOG("Downloaded to /tmp/eacp-download-test.json");

        LOG("");
    }

    // 10. Upload the downloaded file via multipart form-data
    {
        auto req = HTTP::Request("https://httpbin.org/post");
        req.addFormField("description", "Uploaded via eacp")
            .addFileField(
                "file", "/tmp/eacp-download-test.json", "application/json");
        performAndPrint(req);
    }

    LOG("=== All requests completed ===");
    return 0;
}