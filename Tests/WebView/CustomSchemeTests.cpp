#include "Common.h"

#include <cstring>

// Custom schemes on a real WebView: a page loaded from an in-memory scheme
// fetches from it and from a streaming scheme beside it, and posts what
// arrived. The streamed resource is large enough to take many reads, and its
// bytes are a function of their offset, so any slice can be checked whole.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace CustomSchemeTests
{
constexpr auto largeResourceSize = RangeSize {3 * 1024 * 1024 + 17};
constexpr auto byteValueModulus = 251;
constexpr auto rangeStart = RangeSize {1024 * 1024 + 3};
constexpr auto rangeLength = RangeSize {70000};
constexpr auto okStatus = 200;
constexpr auto partialContentStatus = 206;
constexpr auto notFoundStatus = 404;
constexpr auto rangeNotSatisfiableStatus = 416;
constexpr auto fetchTimeout = Time::MS {30000};

const std::string customSchemePageHtml = R"HTML(<!doctype html><html><body><script>
function byteAt(offset) { return offset % 251; }

function firstMismatch(bytes, startOffset) {
  for (let i = 0; i < bytes.length; ++i)
    if (bytes[i] !== byteAt(startOffset + i)) return startOffset + i;
  return -1;
}

async function describe(url, headers) {
  try {
    const response = await fetch(url, { headers: headers || {} });
    const bytes = new Uint8Array(await response.arrayBuffer());
    return {
      status: response.status,
      length: bytes.length,
      contentType: response.headers.get('Content-Type') || '',
      bytes: bytes,
    };
  } catch (e) {
    return { error: String(e) };
  }
}

window.runCase = async function (name, url, headers, startOffset) {
  const result = await describe(url, headers);
  if (result.bytes) {
    result.mismatch = name === 'text' ? -1 : firstMismatch(result.bytes, startOffset);
    result.text = name === 'text' ? new TextDecoder().decode(result.bytes) : '';
    delete result.bytes;
  }
  window.webkit.messageHandlers.result.postMessage(JSON.stringify(result));
};

window.webkit.messageHandlers.ready.postMessage('ready');
</script></body></html>)HTML";

const std::string jsonBody = R"({"served":true})";

std::uint8_t getByteAtOffset(RangeSize offset)
{
    return static_cast<std::uint8_t>(offset % byteValueModulus);
}

std::optional<ResourceResponse> serveMemoryResource(std::string_view url)
{
    auto response = ResourceResponse {};

    if (url.find("/index.html") != std::string_view::npos)
    {
        response.mimeType = "text/html; charset=utf-8";
        response.data.assign(customSchemePageHtml.begin(),
                             customSchemePageHtml.end());
        return response;
    }

    if (url.find("/data.json") != std::string_view::npos)
    {
        response.mimeType = "application/json";
        response.data.assign(jsonBody.begin(), jsonBody.end());
        return response;
    }

    response.mimeType = "text/plain";
    response.statusCode = notFoundStatus;
    return response;
}

std::optional<StreamingResource> servePageAsStream()
{
    auto resource = StreamingResource {};
    resource.mimeType = "text/html; charset=utf-8";
    resource.size = customSchemePageHtml.size();
    resource.read = [](RangeSize offset, ByteSpan out) -> int
    {
        if (offset >= customSchemePageHtml.size())
            return 0;

        auto count =
            std::min<RangeSize>(out.size(), customSchemePageHtml.size() - offset);
        std::memcpy(out.data(), customSchemePageHtml.data() + offset, count);
        return static_cast<int>(count);
    };
    return resource;
}

std::optional<StreamingResource> serveLargeResource(std::string_view url)
{
    if (url.find("/index.html") != std::string_view::npos)
        return servePageAsStream();

    auto resource = StreamingResource {};
    resource.mimeType = "application/octet-stream";
    resource.size = largeResourceSize;
    resource.read = [](RangeSize offset, ByteSpan out) -> int
    {
        if (offset >= largeResourceSize)
            return 0;

        auto count = std::min<RangeSize>(out.size(), largeResourceSize - offset);

        for (auto index = RangeSize {0}; index < count; ++index)
            out[index] = getByteAtOffset(offset + index);

        return static_cast<int>(count);
    };
    return resource;
}

WebView::Options createCustomSchemeOptions()
{
    auto options = WebView::Options {};
    options.schemes["testmemory"] = serveMemoryResource;
    options.streamingSchemes["testlarge"] = serveLargeResource;

    // Each page fetches only from its own scheme: WebView2 and WebKit both
    // fail some fetches across custom schemes.
    //
    // WebView2 refuses an environment that shares a user-data folder with one
    // registering a different scheme set (see StreamingPumpTests).
    options.userDataFolderSuffix = "customscheme";
    return options;
}

const std::string memoryPageUrl = "testmemory://host/index.html";
const std::string streamingPageUrl = "testlarge://host/index.html";
const std::string largeBlobUrl = "testlarge://host/blob";

struct FetchRequest
{
    std::string name;
    std::string url;
    std::string rangeHeader;
    RangeSize startOffset = 0;
};

struct FetchResult
{
    int status = 0;
    int length = 0;
    int mismatch = -1;
    std::string contentType;
    std::string text;
    std::string error;

    MIRO_REFLECT(status, length, mismatch, contentType, text, error)
};

struct CustomSchemeFixture
{
    WebView webView {createCustomSchemeOptions()};
    Window window {};
    bool ready = false;
    Vector<std::string> results;

    explicit CustomSchemeFixture(const std::string& pageUrl)
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.addScriptMessageHandler(
            "result", [this](const std::string& result) { results.add(result); });
        webView.loadURL(pageUrl);
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    FetchResult fetch(const FetchRequest& request)
    {
        auto headers = request.rangeHeader.empty()
                           ? std::string {"{}"}
                           : "{Range: '" + request.rangeHeader + "'}";
        auto expectedCount = results.size() + 1;

        webView.evaluateJavaScript("window.runCase('" + request.name + "', '"
                                   + request.url + "', " + headers + ", "
                                   + std::to_string(request.startOffset) + ")");

        auto arrived = Threads::runEventLoopUntil(
            [this, expectedCount] { return results.size() >= expectedCount; },
            fetchTimeout);
        check(arrived, request.name);

        if (!arrived)
            return {};

        auto result = FetchResult {};
        Miro::fromJSONString(result, results.back());
        return result;
    }
};

std::string createClosedRangeHeader(RangeSize start, RangeSize length)
{
    return "bytes=" + std::to_string(start) + "-"
           + std::to_string(start + length - 1);
}
} // namespace CustomSchemeTests

using namespace CustomSchemeTests;

auto tCustomSchemeMemoryResource =
    test("CustomScheme/memoryResourceArrivesWithItsType") = []
{
    auto fix = CustomSchemeFixture {memoryPageUrl};

    auto result = fix.fetch({"text", "testmemory://host/data.json"});
    check(result.error.empty(), result.error);
    check(result.status == okStatus);
    check(result.text == jsonBody);
    check(result.contentType.find("application/json") != std::string::npos);
};

auto tCustomSchemeStatusCode =
    test("CustomScheme/providerStatusCodeReachesPage") = []
{
    auto fix = CustomSchemeFixture {memoryPageUrl};
    check(fix.fetch({"text", "testmemory://host/missing"}).status == notFoundStatus);
};

auto tCustomSchemeLargeStream =
    test("CustomScheme/largeStreamArrivesWholeAndInOrder") = []
{
    auto fix = CustomSchemeFixture {streamingPageUrl};

    auto result = fix.fetch({"large", largeBlobUrl});
    check(result.error.empty(), result.error);
    check(result.status == okStatus);
    check(result.length == largeResourceSize);
    check(result.mismatch == -1);
};

auto tCustomSchemeClosedRange =
    test("CustomScheme/closedRangeInsideLargeStreamIsExact") = []
{
    auto fix = CustomSchemeFixture {streamingPageUrl};

    auto result = fix.fetch({"range",
                             largeBlobUrl,
                             createClosedRangeHeader(rangeStart, rangeLength),
                             rangeStart});
    check(result.status == partialContentStatus);
    check(result.length == rangeLength);
    check(result.mismatch == -1);
};

auto tCustomSchemeOpenRange = test("CustomScheme/openEndedRangeRunsToTheEnd") = []
{
    auto fix = CustomSchemeFixture {streamingPageUrl};

    auto result = fix.fetch({"open",
                             largeBlobUrl,
                             "bytes=" + std::to_string(rangeStart) + "-",
                             rangeStart});
    check(result.status == partialContentStatus);
    check(result.length == largeResourceSize - rangeStart);
    check(result.mismatch == -1);
};

auto tCustomSchemeUnsatisfiableRange =
    test("CustomScheme/rangePastTheEndIsUnsatisfiable") = []
{
    auto fix = CustomSchemeFixture {streamingPageUrl};

    auto result =
        fix.fetch({"past",
                   largeBlobUrl,
                   "bytes=" + std::to_string(largeResourceSize + 1) + "-"});
    check(result.status == rangeNotSatisfiableStatus);
};
