#include "Common.h"

// What callJS resolves with for each kind of script result on a real WebView.
// WebView2 hands every result back JSON-encoded and the backend decodes it
// (decodeScriptResult); WebKit hands back an object the backend converts. The
// two agree on strings, numbers and objects, which is what these pin.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace ScriptResultTests
{
const std::string scriptResultPageHtml =
    R"HTML(<!doctype html><html><body><script>
window.webkit.messageHandlers.ready.postMessage('ready');
</script></body></html>)HTML";

struct ScriptResultFixture
{
    WebView webView {};
    Window window {};
    bool ready = false;

    ScriptResultFixture()
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.loadHTML(scriptResultPageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    std::string runScript(const std::string& script)
    {
        return webView.callJS(script).waitFor(webViewResultTimeout);
    }
};
} // namespace ScriptResultTests

using namespace ScriptResultTests;

auto tScriptResultString = test("ScriptResult/stringArrivesUnquoted") = []
{
    auto fix = ScriptResultFixture {};
    check(fix.runScript("'abc'") == "abc");
    check(fix.runScript("''").empty());
};

auto tScriptResultStringEscapes =
    test("ScriptResult/stringKeepsQuotesBackslashesAndNewlines") = []
{
    auto fix = ScriptResultFixture {};
    check(fix.runScript(R"('a"b\\c\nd')") == "a\"b\\c\nd");
};

auto tScriptResultUnicode = test("ScriptResult/stringKeepsNonAsciiText") = []
{
    auto fix = ScriptResultFixture {};
    check(fix.runScript("'\\u00e9\\u05d0'") == "\xC3\xA9\xD7\x90");
};

auto tScriptResultJsonText = test("ScriptResult/jsonTextIsNotDecodedTwice") = []
{
    auto fix = ScriptResultFixture {};
    check(fix.runScript("JSON.stringify({a: 1})") == R"({"a":1})");
    check(fix.runScript("JSON.stringify('x')") == "\"x\"");
};

auto tScriptResultNumber = test("ScriptResult/numberArrivesAsDecimalText") = []
{
    auto fix = ScriptResultFixture {};
    check(fix.runScript("40 + 2") == "42");
    check(fix.runScript("1.5") == "1.5");
};

auto tScriptResultObject = test("ScriptResult/objectArrivesAsJson") = []
{
    auto fix = ScriptResultFixture {};

    auto value = Miro::Json::parse(fix.runScript("({text: 'hi', count: 3})"));
    check(value.isObject());
    check(value["text"].asString() == "hi");
    check(value["count"].asInteger() == 3);
};

auto tScriptResultSequence = test("ScriptResult/resultsArriveInCallOrder") = []
{
    auto fix = ScriptResultFixture {};

    constexpr auto callCount = 20;
    auto pending = Vector<Threads::Async<std::string>> {};

    for (auto index = 0; index < callCount; ++index)
        pending.add(fix.webView.callJS("'" + std::to_string(index) + "'"));

    for (auto index = 0; index < callCount; ++index)
        check(pending[index].waitFor(webViewResultTimeout) == std::to_string(index));
};
