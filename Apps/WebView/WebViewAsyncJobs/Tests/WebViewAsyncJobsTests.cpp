#include "App.h"

#include <eacp/WebView/Test/TestApp.h>

#include <NanoTest/NanoTest.h>

using namespace std::chrono_literals;
using namespace eacp::WebView::Test;

using nano::check;

namespace
{

constexpr auto inputSelector = R"([data-testid="search-input"])";

TestApp<MyApp>& testApp()
{
    return createTestApp<MyApp>(inputSelector);
}

Graphics::WebView& webView()
{
    return testApp().app().webView;
}

Async<std::string> callJS(const std::string& expression)
{
    return webView().callJS(expression);
}

// callJS evaluates an expression and returns its value as a string — it
// does NOT await a returned Promise. So we fire the command, stash the
// resolved value on a global, and poll it as JSON. Returns the settled
// value (JSON-encoded), or "null" if it never resolves.
Async<std::string> invokeAndPoll(const std::string& command,
                                 const std::string& payloadJson)
{
    co_await callJS(
        "(function() { window.__r = null; window.eacp.invoke('" + command + "', "
        + payloadJson
        + ").then(function(v) { window.__r = v; },"
          " function(e) { window.__r = { error: String(e) }; }); return 'ok'; })()");

    for (auto attempt = 0; attempt < 100; ++attempt)
    {
        auto settled = co_await callJS("JSON.stringify(window.__r)");
        if (settled != "null")
            co_return settled;

        co_await eacp::Threads::delay(20ms);
    }

    co_return std::string {"null"};
}

// The async search command resolves ~200ms later with hits derived from
// the query — proving the bridge defers its reply through the awaitable
// and round-trips the resulting vector<SearchHit>.
auto tSearchResolves =
    test("WebViewAsyncJobs/searchResolvesWithHitsAfterDelay") = []() -> Async<>
{
    auto settled = co_await invokeAndPoll("search", R"({ "query": "hello" })");

    check(settled.find(R"("query":"hello")") != std::string::npos);
    // Titles are "<query> — result N", so a hit title embeds the query.
    check(settled.find(R"("title":"hello )") != std::string::npos);
    check(settled.find(R"("score":)") != std::string::npos);
};

// The synchronous sibling registers identically and round-trips the same
// result shape — it just replies inline (after blocking) rather than via
// a deferred continuation.
auto tBlockingSearch =
    test("WebViewAsyncJobs/blockingSearchReturnsHits") = []() -> Async<>
{
    auto settled = co_await invokeAndPoll("searchBlocking", R"({ "query": "sync" })");

    check(settled.find(R"("query":"sync")") != std::string::npos);
    check(settled.find(R"("title":"sync )") != std::string::npos);
};

// An empty query short-circuits to zero hits — still asynchronously.
auto tEmptyQuery =
    test("WebViewAsyncJobs/emptyQueryReturnsNoHits") = []() -> Async<>
{
    auto settled = co_await invokeAndPoll("search", R"({ "query": "" })");

    check(settled.find(R"("hits":[])") != std::string::npos);
};

// The deferral itself: right after invoke(), while the coroutine is still
// inside its 200ms delay, nothing has been delivered yet.
auto tStaysPending =
    test("WebViewAsyncJobs/searchIsPendingBeforeItSettles") = []() -> Async<>
{
    co_await callJS(
        "(function() {"
        "   window.__p = null;"
        "   window.eacp.invoke('search', { query: 'slow' })"
        "     .then(function(v) { window.__p = v; });"
        "   return 'ok';"
        " })()");

    auto immediate = co_await callJS("JSON.stringify(window.__p)");
    check(immediate == "null");
};

} // namespace
