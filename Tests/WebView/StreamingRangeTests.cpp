#include "Common.h"
#include <eacp/WebView/WebView/StreamingRange.h>
#include <eacp/WebView/WebView/WebViewDetail.h>

using namespace nano;
using namespace eacp::Graphics;

namespace
{
StreamingResource resourceOfSize(RangeSize size)
{
    auto resource = StreamingResource {};
    resource.mimeType = "text/plain";
    resource.size = size;
    return resource;
}

std::string headerValue(const StreamingResponsePlan& plan, const char* name)
{
    for (const auto& [key, value]: plan.headers)
        if (key == name)
            return value;
    return {};
}

bool hasHeader(const StreamingResponsePlan& plan, const char* name)
{
    for (const auto& [key, value]: plan.headers)
        if (key == name)
            return true;
    return false;
}
} // namespace

auto tNoHeaderIsFull = test("StreamingRange/noHeaderFull") = []
{
    auto r = resolveRangeHeader("", 1000);
    check(r.kind == RangeRequest::Full);
    check(r.served.start == 0);
    check(r.served.length == 1000);
};

auto tOpenEndedRange = test("StreamingRange/openEnded") = []
{
    auto r = resolveRangeHeader("bytes=200-", 1000);
    check(r.kind == RangeRequest::Partial);
    check(r.served.start == 200);
    check(r.served.length == 800);
};

auto tClosedRange = test("StreamingRange/closed") = []
{
    auto r = resolveRangeHeader("bytes=0-499", 1000);
    check(r.kind == RangeRequest::Partial);
    check(r.served.start == 0);
    check(r.served.length == 500);
};

auto tClosedRangeClampsToSize = test("StreamingRange/closedClamps") = []
{
    auto r = resolveRangeHeader("bytes=900-100000", 1000);
    check(r.kind == RangeRequest::Partial);
    check(r.served.start == 900);
    check(r.served.length == 100); // bytes 900..999
};

auto tSuffixRange = test("StreamingRange/suffix") = []
{
    auto r = resolveRangeHeader("bytes=-300", 1000);
    check(r.kind == RangeRequest::Partial);
    check(r.served.start == 700);
    check(r.served.length == 300);
};

auto tSuffixLargerThanSize = test("StreamingRange/suffixLargerThanSize") = []
{
    auto r = resolveRangeHeader("bytes=-5000", 1000);
    check(r.kind == RangeRequest::Partial);
    check(r.served.start == 0);
    check(r.served.length == 1000);
};

auto tUnsatisfiable = test("StreamingRange/unsatisfiable") = []
{
    check(resolveRangeHeader("bytes=2000-3000", 1000).kind
          == RangeRequest::Unsatisfiable);
};

auto tMultiRangeFallsBackToFull = test("StreamingRange/multiRangeFull") = []
{
    check(resolveRangeHeader("bytes=0-99,200-299", 1000).kind == RangeRequest::Full);
};

auto tMalformedFallsBackToFull = test("StreamingRange/malformedFull") = []
{
    check(resolveRangeHeader("bytes=abc", 1000).kind == RangeRequest::Full);
    check(resolveRangeHeader("items=0-10", 1000).kind == RangeRequest::Full);
};

auto tEmptyResourceUnsatisfiable = test("StreamingRange/emptyResource") = []
{ check(resolveRangeHeader("bytes=0-10", 0).kind == RangeRequest::Unsatisfiable); };

auto tContentRangeValue = test("StreamingRange/contentRangeValue") = []
{
    check(contentRangeValue(ByteRange {0, 500}, 1000) == "bytes 0-499/1000");
    check(contentRangeValue(ByteRange {200, 800}, 1000) == "bytes 200-999/1000");
};

auto tPlanFull = test("StreamingPlan/fullServes200") = []
{
    auto plan = planStreamingResponse("", resourceOfSize(1000));
    check(plan.statusCode == 200);
    check(plan.hasBody);
    check(plan.served.start == 0);
    check(plan.served.length == 1000);
    check(headerValue(plan, "Content-Type") == "text/plain");
    check(headerValue(plan, "Accept-Ranges") == "bytes");
    check(headerValue(plan, "Access-Control-Allow-Origin") == "*");
    check(headerValue(plan, "Content-Length") == "1000");
    check(!hasHeader(plan, "Content-Range"));
};

auto tPlanPartial = test("StreamingPlan/partialServes206") = []
{
    auto plan = planStreamingResponse("bytes=0-499", resourceOfSize(1000));
    check(plan.statusCode == 206);
    check(plan.hasBody);
    check(plan.served.start == 0);
    check(plan.served.length == 500);
    check(headerValue(plan, "Content-Range") == "bytes 0-499/1000");
    check(headerValue(plan, "Content-Length") == "500");
    check(headerValue(plan, "Access-Control-Allow-Origin") == "*");
};

auto tPlanUnsatisfiable = test("StreamingPlan/unsatisfiableServes416") = []
{
    auto plan = planStreamingResponse("bytes=2000-3000", resourceOfSize(1000));
    check(plan.statusCode == 416);
    check(!plan.hasBody);
    check(headerValue(plan, "Content-Range") == "bytes */1000");
    check(!hasHeader(plan, "Content-Length"));
};

auto tPlanEmptyResource = test("StreamingPlan/emptyResourceNoBody") = []
{
    auto plan = planStreamingResponse("", resourceOfSize(0));
    check(plan.statusCode == 200);
    check(!plan.hasBody);
    check(headerValue(plan, "Content-Length") == "0");
};

auto tPlanKeepsResourceStatus = test("StreamingPlan/keepsResourceStatus") = []
{
    auto resource = resourceOfSize(1000);
    resource.statusCode = 203;
    auto plan = planStreamingResponse("", resource);
    check(plan.statusCode == 203);
};

auto tClampZoom = test("WebViewZoom/clamps") = []
{
    check(detail::clampZoom(0.1) == 0.25);
    check(detail::clampZoom(10.0) == 5.0);
    check(detail::clampZoom(1.5) == 1.5);
    check(detail::clampZoom(0.25) == 0.25);
    check(detail::clampZoom(5.0) == 5.0);
};
