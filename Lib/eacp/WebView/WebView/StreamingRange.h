#pragma once

#include "WebView.h"

#include <string>
#include <string_view>

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
} // namespace eacp::Graphics
