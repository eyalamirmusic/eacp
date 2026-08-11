#pragma once

#include "../Common.h"

#include "WebView.h"

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

// Parses "bytes=0-1023", "bytes=500-", "bytes=-500". Empty, malformed and
// multi-range headers all resolve to Full.
ResolvedRange resolveRangeHeader(std::string_view headerValue, RangeSize size);

// e.g. "bytes 0-1023/2048". Only valid for a non-empty served range.
std::string contentRangeValue(const ByteRange& served, RangeSize size);

// Platform-neutral response description each backend turns into its own type.
struct StreamingResponsePlan
{
    int statusCode = 200;
    Vector<std::pair<std::string, std::string>> headers;
    ByteRange served {};
    bool hasBody = false;
};

StreamingResponsePlan planStreamingResponse(std::string_view rangeHeader,
                                            const StreamingResource& resource);
} // namespace eacp::Graphics
