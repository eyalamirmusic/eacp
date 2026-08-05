#include "Common.h"

using namespace nano;
using eacp::HTTP::ServerSentEvent;
using eacp::HTTP::SseParser;

namespace
{
struct Collector
{
    SseParser parser;
    eacp::Vector<ServerSentEvent> events;

    Collector()
    {
        parser.onEvent = [this](const ServerSentEvent& e) { events.add(e); };
    }

    void feed(std::string_view chunk) { parser.feed(chunk); }

    // check() records a failure and carries on, so a count assertion that
    // fails would otherwise be followed by an out-of-range index and take
    // the whole binary down with it. Reading through here turns that into
    // a reported failure.
    ServerSentEvent at(int index) const
    {
        return index < events.size() ? events[index] : ServerSentEvent();
    }
};
} // namespace

auto tSseSingleEvent = test("Sse/dispatchesOnBlankLine") = []
{
    auto c = Collector();
    c.feed("data: hello\n\n");

    check(c.events.size() == 1);
    check(c.at(0).data == "hello");
    check(c.at(0).event == "message");
};

auto tSseNoDispatchWithoutBlankLine = test("Sse/waitsForBlankLine") = []
{
    auto c = Collector();
    c.feed("data: hello\n");

    check(c.events.empty());
};

auto tSseNamedEvent = test("Sse/readsEventName") = []
{
    auto c = Collector();
    c.feed("event: content_block_delta\ndata: {\"x\":1}\n\n");

    check(c.events.size() == 1);
    check(c.at(0).event == "content_block_delta");
    check(c.at(0).data == "{\"x\":1}");
};

auto tSseEventNameResets = test("Sse/eventNameResetsBetweenEvents") = []
{
    auto c = Collector();
    c.feed("event: first\ndata: a\n\ndata: b\n\n");

    check(c.events.size() == 2);
    check(c.at(0).event == "first");
    check(c.at(1).event == "message");
};

auto tSseMultiLineData = test("Sse/joinsMultipleDataLines") = []
{
    auto c = Collector();
    c.feed("data: one\ndata: two\n\n");

    check(c.events.size() == 1);
    check(c.at(0).data == "one\ntwo");
};

// A trailing empty data field is how a stream encodes a blank line inside a
// payload; joining with a separator instead of appending one per line would
// silently drop it.
auto tSseTrailingEmptyDataLine = test("Sse/keepsBlankLineInsideData") = []
{
    auto c = Collector();
    c.feed("data: one\ndata:\n\n");

    check(c.events.size() == 1);
    check(c.at(0).data == "one\n");
};

auto tSseStripsOneLeadingSpace = test("Sse/stripsExactlyOneLeadingSpace") = []
{
    auto c = Collector();
    c.feed("data:  two spaces\n\n");

    check(c.events.size() == 1);
    check(c.at(0).data == " two spaces");
};

auto tSseFieldWithoutColon = test("Sse/handlesFieldWithNoColon") = []
{
    auto c = Collector();
    c.feed("data\n\n");

    check(c.events.size() == 1);
    check(c.at(0).data.empty());
};

auto tSseIgnoresComments = test("Sse/ignoresCommentLines") = []
{
    auto c = Collector();
    c.feed(": keep-alive\ndata: real\n\n");

    check(c.events.size() == 1);
    check(c.at(0).data == "real");
};

auto tSseSkipsEmptyDataEvents = test("Sse/dropsEventsWithNoDataField") = []
{
    auto c = Collector();
    c.feed("event: ping\n\ndata: real\n\n");

    check(c.events.size() == 1);
    check(c.at(0).data == "real");
};

auto tSseReadsId = test("Sse/readsIdAndCarriesItForward") = []
{
    auto c = Collector();
    c.feed("id: 42\ndata: a\n\ndata: b\n\n");

    check(c.events.size() == 2);
    check(c.at(0).id == "42");
    check(c.at(1).id == "42");
};

auto tSseReadsRetry = test("Sse/readsNumericRetryOnly") = []
{
    auto c = Collector();
    c.feed("retry: 2500\ndata: a\n\nretry: soon\ndata: b\n\n");

    check(c.events.size() == 2);
    check(c.at(0).retryMs.has_value());
    check(*c.at(0).retryMs == 2500);
    check(!c.at(1).retryMs.has_value());
};

auto tSseCrlf = test("Sse/handlesCrlfLineEndings") = []
{
    auto c = Collector();
    c.feed("event: e\r\ndata: v\r\n\r\n");

    check(c.events.size() == 1);
    check(c.at(0).event == "e");
    check(c.at(0).data == "v");
};

auto tSseBareCr = test("Sse/handlesBareCrLineEndings") = []
{
    auto c = Collector();
    c.feed("data: v\r\rdata: w\r\r");

    check(c.events.size() == 1);
    check(c.at(0).data == "v");
};

// A carriage return that lands on the end of a chunk cannot yet be told
// apart from the first half of a CRLF, so it is held back. Nothing but more
// bytes or end-of-stream can resolve it, and both do.
auto tSseTrailingCrHeldUntilResolved = test("Sse/deferstrailingCarriageReturn") = []
{
    auto c = Collector();
    c.feed("data: v\r\r");

    check(c.events.empty());

    c.parser.finish();

    check(c.events.size() == 1);
    check(c.at(0).data == "v");
};

// The case a live stream hits and a whole-body test never does: the reader
// must not treat a chunk boundary as a line boundary.
auto tSseSplitAcrossChunks = test("Sse/reassemblesLineSplitAcrossChunks") = []
{
    auto c = Collector();
    c.feed("data: hel");
    check(c.events.empty());

    c.feed("lo\n\n");

    check(c.events.size() == 1);
    check(c.at(0).data == "hello");
};

auto tSseSplitCrlf = test("Sse/reassemblesCrlfSplitAcrossChunks") = []
{
    auto c = Collector();
    c.feed("data: v\r");
    check(c.events.empty());

    c.feed("\n\r\n");

    check(c.events.size() == 1);
    check(c.at(0).data == "v");
};

auto tSseBytewise = test("Sse/survivesOneBytePerChunk") = []
{
    auto c = Collector();
    auto stream = std::string("event: tick\ndata: 1\n\ndata: 2\n\n");

    for (auto ch: stream)
        c.feed(std::string_view(&ch, 1));

    check(c.events.size() == 2);
    check(c.at(0).event == "tick");
    check(c.at(0).data == "1");
    check(c.at(1).data == "2");
};

auto tSseFinishFlushes = test("Sse/finishFlushesUnterminatedEvent") = []
{
    auto c = Collector();
    c.feed("data: last\n");
    check(c.events.empty());

    c.parser.finish();

    check(c.events.size() == 1);
    check(c.at(0).data == "last");
};

auto tSseFinishDropsPartialLine = test("Sse/finishDiscardsPartialLine") = []
{
    auto c = Collector();
    c.feed("data: complete\n");
    c.feed("data: trunc");

    c.parser.finish();

    check(c.events.size() == 1);
    check(c.at(0).data == "complete");
};

auto tSseReset = test("Sse/resetDropsPendingState") = []
{
    auto c = Collector();
    c.feed("data: pending\n");
    c.parser.reset();
    c.parser.finish();

    check(c.events.empty());
};
