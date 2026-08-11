#include "Common.h"
#include <algorithm>
using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto streamingResultTimeout = eacp::Time::MS {30000};

// 26 bytes; a closed range pulls out a known slice.
const std::string streamData = "abcdefghijklmnopqrstuvwxyz";

const std::string pageHtml = R"HTML(<!doctype html><html><body><script>
(async function () {
  try {
    const r = await fetch('teststream://host/data', { headers: { Range: 'bytes=2-5' } });
    const body = await r.text();
    window.webkit.messageHandlers.result.postMessage(JSON.stringify({
      status: r.status,
      contentRange: r.headers.get('Content-Range'),
      acceptRanges: r.headers.get('Accept-Ranges'),
      body: body,
    }));
  } catch (e) {
    window.webkit.messageHandlers.result.postMessage(
        JSON.stringify({ error: String(e) }));
  }
})();
</script></body></html>)HTML";

StreamingProvider testProvider()
{
    return [](std::string_view url) -> std::optional<StreamingResource>
    {
        auto isData = url.find("/data") != std::string_view::npos;
        const auto* payload = isData ? &streamData : &pageHtml;

        auto resource = StreamingResource {};
        resource.mimeType =
            isData ? "application/octet-stream" : "text/html; charset=utf-8";
        resource.size = payload->size();
        resource.read = [payload](RangeSize offset, ByteSpan out) -> std::size_t
        {
            if (offset >= payload->size())
                return 0;

            auto available = payload->size() - static_cast<std::size_t>(offset);
            auto count = std::min(out.size(), available);
            std::memcpy(out.data(), payload->data() + offset, count);
            return count;
        };
        return resource;
    };
}
} // namespace

auto tStreamingRangeFetch = test("StreamingPump/rangeFetchReturns206Slice") = []
{
    auto options = WebView::Options {};
    options.streamingSchemes["teststream"] = testProvider();

    // The only suite registering a custom scheme: WebView2 fails any later
    // environment sharing a user-data folder with a different scheme set
    // (ERROR_INVALID_STATE), so this one needs its own folder.
    options.userDataFolderSuffix = "streamingpump";

    auto webView = WebView {options};
    auto window = Window {};
    window.setContentView(webView);

    auto done = false;
    auto message = std::string {};
    webView.addScriptMessageHandler("result",
                                    [&](const std::string& m)
                                    {
                                        message = m;
                                        done = true;
                                    });

    webView.loadURL("teststream://host/index.html");

    auto ok =
        Threads::runEventLoopUntil([&] { return done; }, streamingResultTimeout);

    check(ok);
    check(message.find(R"("status":206)") != std::string::npos);
    check(message.find(R"("body":"cdef")") != std::string::npos);
    check(message.find("bytes 2-5/26") != std::string::npos);
    check(message.find(R"("acceptRanges":"bytes")") != std::string::npos);
};
