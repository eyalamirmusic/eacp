#include <eacp/Core/App/Args.h>
#include <eacp/Core/Core.h>
#include <eacp/Network/Network.h>

using namespace eacp;

// The HTTP client's blocking calls, one of each kind, against an
// httpbin-compatible server: httpbingo.org by default, which sends CORS
// headers, so the same requests run in a browser page too. Give another
// server's URL as the argument (ctest gives Tests/Network/Web/TestServer.mjs).
// Exits 1 if any request failed.
namespace
{
int failures = 0;

void printResponse(const HTTP::Response& res)
{
    LOG("Status: ", res.statusCode);

    if (!res.error.empty())
        LOG("Error: " + res.error);
    else
        LOG("Content: " + res.content);

    if (!res.error.empty() || res.statusCode < 200 || res.statusCode >= 300)
        ++failures;

    LOG("");
}

void performAndPrint(const HTTP::Request& req)
{
    printResponse(req.perform());
}

void getAndPrint(const std::string& url)
{
    printResponse(HTTP::httpRequest(url));
}
} // namespace

int main(int argc, char** argv)
{
    auto server = std::string {"https://httpbingo.org"};

    auto cli = Args::Parser {"Http", "One request of each kind, printed."};
    cli.positional(server, "server", "an httpbin-compatible server");

    if (auto code = cli.exitCodeFor(cli.parse(argc, argv)))
        return *code;

    while (server.ends_with("/"))
        server.pop_back();

    // 1. A JSON document
    getAndPrint(server + "/json");

    // 2. GET with a query string
    getAndPrint(server + "/anything?userId=1");

    // 3. GET with custom headers
    {
        auto req = HTTP::Request(server + "/headers");
        req.headers["X-Custom-Header"] = "CustomValue";
        req.headers["Accept"] = "application/json";
        performAndPrint(req);
    }

    // 4. POST a JSON body
    {
        auto jsonBody = R"({
            "title": "My New Post",
            "body": "This is the content of my post",
            "userId": 1
        })";
        auto req = HTTP::Request::post(server + "/post", jsonBody);
        req.headers["Content-Type"] = "application/json";
        performAndPrint(req);
    }

    // 5. POST form data
    {
        auto formData = "username=testuser&password=secret123";
        auto req = HTTP::Request::post(server + "/post", formData);
        req.headers["Content-Type"] = "application/x-www-form-urlencoded";
        performAndPrint(req);
    }

    // 6. GET user agent info
    {
        auto req = HTTP::Request(server + "/user-agent");
        req.headers["User-Agent"] = "eacp-http-client/1.0";
        performAndPrint(req);
    }

    // 7. The caller's address as the server sees it
    getAndPrint(server + "/ip");

    // 8. GET with authentication header (Bearer token simulation)
    {
        auto req = HTTP::Request(server + "/bearer");
        req.headers["Authorization"] = "Bearer my-fake-jwt-token-12345";
        performAndPrint(req);
    }

    const auto downloaded = FilePath::tempDirectory() / "eacp-download-test.json";

    // 9. Download a file to disk
    {
        auto req = HTTP::Request(server + "/json");
        auto res = req.downloadTo(downloaded.str());

        LOG("Download status: ", res.statusCode);

        if (!res.error.empty())
        {
            LOG("Download error: " + res.error);
            ++failures;
        }
        else
            LOG("Downloaded to ", downloaded.str());

        LOG("");
    }

    // 10. Upload the downloaded file via multipart form-data
    {
        auto req = HTTP::Request(server + "/post");
        req.addFormField("description", "Uploaded via eacp")
            .addFileField("file", downloaded.str(), "application/json");
        performAndPrint(req);
    }

    LOG("=== All requests completed, ", failures, " failed ===");
    return failures == 0 ? 0 : 1;
}
