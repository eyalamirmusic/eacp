#include "Common.h"
#include <eacp/WebView/WebView/WebViewMessages.h>

using namespace nano;
using namespace eacp::Graphics::detail;

namespace WebViewMessagesTests
{
constexpr auto escapeVirtualKey = 27;
constexpr auto letterAVirtualKey = 65;
constexpr auto shiftAndCommandMask = "9";
constexpr auto okStatus = 200;
constexpr auto partialStatus = 206;
constexpr auto unsatisfiableStatus = 416;
constexpr auto notFoundStatus = 404;
constexpr auto createdStatus = 201;
constexpr auto redirectStatus = 302;
} // namespace WebViewMessagesTests

using namespace WebViewMessagesTests;

auto tKeyVerdictReadsEveryField =
    test("WebViewMessages/keyVerdictReadsEveryField") = []
{
    auto verdict =
        parseKeyVerdict(std::string {"down:0:65:"} + shiftAndCommandMask + ":1:a");
    check(verdict.has_value());
    check(verdict->isDown);
    check(!verdict->consumed);
    check(verdict->virtualKey == letterAVirtualKey);
    check(verdict->modifiers.shift);
    check(!verdict->modifiers.control);
    check(!verdict->modifiers.alt);
    check(verdict->modifiers.command);
    check(verdict->isRepeat);
    check(verdict->characters == "a");
};

auto tKeyVerdictKeepsColonKey = test("WebViewMessages/keyVerdictKeepsColonKey") = []
{
    auto verdict = parseKeyVerdict("up:1:186:0:0::");
    check(verdict.has_value());
    check(!verdict->isDown);
    check(verdict->consumed);
    check(verdict->characters == ":");
};

auto tKeyVerdictNamedKeyHasNoCharacters =
    test("WebViewMessages/keyVerdictNamedKeyHasNoCharacters") = []
{
    auto verdict = parseKeyVerdict("down:0:27:0:0:Escape");
    check(verdict.has_value());
    check(verdict->virtualKey == escapeVirtualKey);
    check(verdict->characters.empty());
};

auto tKeyVerdictNonAsciiKeyIsText =
    test("WebViewMessages/keyVerdictNonAsciiKeyIsText") = []
{
    auto verdict = parseKeyVerdict("down:0:0:0:0:\xC3\xA9");
    check(verdict.has_value());
    check(verdict->characters == "\xC3\xA9");
};

auto tKeyVerdictShortMessageDefaults =
    test("WebViewMessages/keyVerdictShortMessageDefaults") = []
{
    auto verdict = parseKeyVerdict("down");
    check(verdict.has_value());
    check(!verdict->consumed);
    check(verdict->virtualKey == 0);
    check(!verdict->isRepeat);
    check(verdict->characters.empty());
};

auto tKeyVerdictRejectsUnknownKind =
    test("WebViewMessages/keyVerdictRejectsUnknownKind") = []
{
    check(!parseKeyVerdict("press:0:65:0:0:a").has_value());
    check(!parseKeyVerdict("").has_value());
};

auto tEnvelopeStringBody = test("WebViewMessages/envelopeStringBody") = []
{
    auto message = parseWebMessageEnvelope(R"({"name":"probe","body":"hi"})");
    check(message.has_value());
    check(message->name == "probe");
    check(message->body == "hi");
};

auto tEnvelopeObjectBodyIsPrinted =
    test("WebViewMessages/envelopeObjectBodyIsPrinted") = []
{
    auto message =
        parseWebMessageEnvelope(R"({"name":"probe","body":{"type":"ready"}})");
    check(message.has_value());
    check(message->body.find("\"type\"") != std::string::npos);
    check(message->body.find("\"ready\"") != std::string::npos);
};

auto tEnvelopeScalarBodyIsEmpty =
    test("WebViewMessages/envelopeScalarBodyIsEmpty") = []
{
    auto message = parseWebMessageEnvelope(R"({"name":"probe","body":42})");
    check(message.has_value());
    check(message->body.empty());
};

auto tEnvelopeNonObjectHasNoName =
    test("WebViewMessages/envelopeNonObjectHasNoName") = []
{
    auto message = parseWebMessageEnvelope("[1,2]");
    check(message.has_value());
    check(message->name.empty());
};

auto tEnvelopeRejectsBadJson = test("WebViewMessages/envelopeRejectsBadJson") = []
{ check(!parseWebMessageEnvelope("{not json").has_value()); };

auto tScriptResultDecodesString =
    test("WebViewMessages/scriptResultDecodesString") = []
{
    check(decodeScriptResult("\"abc\"") == "abc");
    check(decodeScriptResult("42") == "42");
    check(decodeScriptResult("{\"a\":1}") == "{\"a\":1}");
    check(decodeScriptResult("not json") == "not json");
};

auto tUrlScheme = test("WebViewMessages/urlScheme") = []
{
    check(getUrlScheme("app://local/index.html")
          == std::optional<std::string> {"app"});
    check(!getUrlScheme("about:blank").has_value());
};

auto tStatusReasons = test("WebViewMessages/statusReasons") = []
{
    check(getStreamingStatusReason(okStatus) == "OK");
    check(getStreamingStatusReason(partialStatus) == "Partial Content");
    check(getStreamingStatusReason(unsatisfiableStatus) == "Range Not Satisfiable");
    check(getResourceStatusReason(createdStatus) == "OK");
    check(getResourceStatusReason(redirectStatus) == "Error");
    check(getResourceStatusReason(notFoundStatus) == "Error");
};

auto tJoinResponseHeaders = test("WebViewMessages/joinResponseHeaders") = []
{
    auto headers = eacp::Vector<std::pair<std::string, std::string>> {};
    check(joinResponseHeaders(headers).empty());

    headers.add({"Content-Type", "text/plain"});
    headers.add({"Accept-Ranges", "bytes"});
    check(joinResponseHeaders(headers)
          == "Content-Type: text/plain\r\nAccept-Ranges: bytes");
};

auto tMessageHandlerScript = test("WebViewMessages/messageHandlerScript") = []
{
    auto script = buildMessageHandlerScript("probe");
    check(script.find("{name:'probe',body:msg}") != std::string::npos);
    check(script.find("window.probe={postMessage:send}") != std::string::npos);
    check(script.find("window.webkit.messageHandlers.probe={postMessage:send}")
          != std::string::npos);
};

auto tMaximizedStateScript = test("WebViewMessages/maximizedStateScript") = []
{
    check(buildMaximizedStateScript(true) == "window.__eacpSetMaximized(true)");
    check(buildMaximizedStateScript(false) == "window.__eacpSetMaximized(false)");
};

auto tKeyVerdictNonNumericFieldsReadAsZero =
    test("WebViewMessages/keyVerdictNonNumericFieldsReadAsZero") = []
{
    auto verdict = parseKeyVerdict("down:0:abc::0:a");
    check(verdict.has_value());
    check(verdict->virtualKey == 0);
    check(!verdict->modifiers.shift);
    check(verdict->characters == "a");
};

auto tKeyVerdictKeepsColonsInKey =
    test("WebViewMessages/keyVerdictKeepsColonsInKey") = []
{
    auto verdict = parseKeyVerdict("down:0:0:0:0:a:b");
    check(verdict.has_value());
    check(verdict->characters.empty());
};
