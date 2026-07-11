#include "Common.h"
// Drives the macOS streaming scheme handler end-to-end on a real WKWebView:
// a page loaded from a custom streaming scheme issues a ranged fetch() against
// a sibling URL on the same scheme, and posts the status / headers / body back
// over a script message handler. This exercises the actual chunked pump (the
// background read -> main-thread didReceiveData loop) and the 206 / Range
// header path, not just the pure resolveRangeHeader logic.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;
using namespace std::chrono_literals;

namespace
{
constexpr auto streamingResultTimeout = 30s;

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
