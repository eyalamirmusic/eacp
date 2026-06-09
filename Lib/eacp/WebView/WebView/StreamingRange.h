#pragma once

#include "WebView.h"

#include <string>
#include <string_view>
#include <utility>

namespace eacp::Graphics
{
enum class RangeRequest
{
    Full, // no (parseable) Range header -> serve the whole resource (200)
    Partial, // a satisfiable byte range -> serve part of it (206)
    Unsatisfiable, // a range that falls outside the resource -> 416
};

struct ResolvedRange
{
    RangeRequest kind = RangeRequest::Full;
    ByteRange served {}; // bytes to deliver; for Full this is the whole resource
};

// Resolves an HTTP `Range` request-header value (e.g. "bytes=0-1023",
// "bytes=500-", "bytes=-500") against a known resource `size`. An empty or
// malformed header -- or any multi-range request, which we don't serve --
// resolves to Full.
ResolvedRange resolveRangeHeader(std::string_view headerValue, RangeSize size);

// The `Content-Range` response-header value for a partial response, e.g.
// "bytes 0-1023/2048". Only valid for a non-empty served range.
std::string contentRangeValue(const ByteRange& served, RangeSize size);

// The decision layer above the range math: which status code, response headers,
// served byte range, and whether a body follows. Each backend translates this
// into its native response type (NSHTTPURLResponse / ICoreWebView2*Response).
struct StreamingResponsePlan
{
    int statusCode = 200;
    Vector<std::pair<std::string, std::string>> headers; // name -> value
    ByteRange served {};
    bool hasBody = false;
};

// Resolves the request's `Range` header against the resource and builds the
// response plan: 200 / 206 / 416 with the matching Content-Type, Accept-Ranges,
// Content-Range, Content-Length, and CORS headers.
StreamingResponsePlan planStreamingResponse(std::string_view rangeHeader,
                                            const StreamingResource& resource);
} // namespace eacp::Graphics
