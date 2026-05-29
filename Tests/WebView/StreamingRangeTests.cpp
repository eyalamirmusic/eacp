#include <eacp/WebView/WebView/StreamingRange.h>
#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp::Graphics;

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
