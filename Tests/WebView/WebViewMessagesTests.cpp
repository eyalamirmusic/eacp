#include <NanoTest/NanoTest.h>

#include <eacp/WebView/WebView/WebViewMessages.h>

using namespace nano;
using namespace eacp::Graphics;
using namespace eacp::Graphics::detail;

namespace WebViewMessagesTests
{
constexpr auto escapeVirtualKey = 27;
constexpr auto letterAVirtualKey = 65;
constexpr auto pageUpVirtualKey = 33;
constexpr auto shiftModifierMask = 1;
constexpr auto controlModifierMask = 2;
constexpr auto altModifierMask = 4;
constexpr auto commandModifierMask = 8;

std::string makeKeyReport(const std::string& fields)
{
    return "{\"kind\":\"down\"," + fields + "}";
}

ModifierKeys readModifiersFromMask(int mask)
{
    auto verdict =
        parseKeyVerdict(makeKeyReport("\"modifiers\":" + std::to_string(mask)));
    check(verdict.has_value());
    return verdict ? verdict->modifiers : ModifierKeys {};
}
} // namespace WebViewMessagesTests

using namespace WebViewMessagesTests;

auto tKeyVerdictReadsEveryField =
    test("WebViewMessages/keyVerdictReadsEveryField") = []
{
    auto verdict = parseKeyVerdict(
        R"({"kind":"down","consumed":false,"keyCode":65,"modifiers":9,"repeat":true,"key":"a"})");
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

auto tKeyVerdictColonKey = test("WebViewMessages/keyVerdictColonKey") = []
{
    auto verdict = parseKeyVerdict(
        R"({"kind":"up","consumed":true,"keyCode":186,"modifiers":0,"repeat":false,"key":":"})");
    check(verdict.has_value());
    check(!verdict->isDown);
    check(verdict->consumed);
    check(verdict->characters == ":");
};

auto tKeyVerdictNamedKeyHasNoCharacters =
    test("WebViewMessages/keyVerdictNamedKeyHasNoCharacters") = []
{
    auto verdict = parseKeyVerdict(
        R"({"kind":"down","consumed":false,"keyCode":27,"modifiers":0,"repeat":false,"key":"Escape"})");
    check(verdict.has_value());
    check(verdict->virtualKey == escapeVirtualKey);
    check(verdict->characters.empty());
};

auto tKeyVerdictNonAsciiKeyIsText =
    test("WebViewMessages/keyVerdictNonAsciiKeyIsText") = []
{
    auto verdict = parseKeyVerdict("{\"kind\":\"down\",\"key\":\"\xC3\xA9\"}");
    check(verdict.has_value());
    check(verdict->characters == "\xC3\xA9");
};

auto tKeyVerdictMissingFieldsDefault =
    test("WebViewMessages/keyVerdictMissingFieldsDefault") = []
{
    auto verdict = parseKeyVerdict(R"({"kind":"down"})");
    check(verdict.has_value());
    check(!verdict->consumed);
    check(verdict->virtualKey == 0);
    check(!verdict->isRepeat);
    check(verdict->characters.empty());
};

auto tKeyVerdictRejectsUnknownKind =
    test("WebViewMessages/keyVerdictRejectsUnknownKind") = []
{
    check(!parseKeyVerdict(R"({"kind":"press","key":"a"})").has_value());
    check(!parseKeyVerdict(R"({"key":"a"})").has_value());
};

auto tKeyVerdictRejectsInvalidJson =
    test("WebViewMessages/keyVerdictRejectsInvalidJson") = []
{
    check(!parseKeyVerdict("").has_value());
    check(!parseKeyVerdict("down:0:65:0:0:a").has_value());
    check(!parseKeyVerdict("[\"down\"]").has_value());
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

auto tKeyVerdictEachModifierBitIsItsOwnKey =
    test("WebViewMessages/keyVerdictEachModifierBitIsItsOwnKey") = []
{
    auto shift = readModifiersFromMask(shiftModifierMask);
    check(shift.shift && !shift.control && !shift.alt && !shift.command);

    auto control = readModifiersFromMask(controlModifierMask);
    check(!control.shift && control.control && !control.alt && !control.command);

    auto alt = readModifiersFromMask(altModifierMask);
    check(!alt.shift && !alt.control && alt.alt && !alt.command);

    auto command = readModifiersFromMask(commandModifierMask);
    check(!command.shift && !command.control && !command.alt && command.command);
};

auto tKeyVerdictNavigationKeyHasNoCharacters =
    test("WebViewMessages/keyVerdictNavigationKeyHasNoCharacters") = []
{
    auto verdict = parseKeyVerdict(makeKeyReport(R"("keyCode":33,"key":"PageUp")"));
    check(verdict.has_value());
    check(verdict->virtualKey == pageUpVirtualKey);
    check(verdict->characters.empty());
};

auto tKeyVerdictSpaceIsText = test("WebViewMessages/keyVerdictSpaceIsText") = []
{
    auto verdict = parseKeyVerdict(makeKeyReport(R"("keyCode":32,"key":" ")"));
    check(verdict.has_value());
    check(verdict->characters == " ");
};

auto tKeyVerdictDeadKeyHasNoCharacters =
    test("WebViewMessages/keyVerdictDeadKeyHasNoCharacters") = []
{
    auto verdict = parseKeyVerdict(makeKeyReport(R"("keyCode":222,"key":"Dead")"));
    check(verdict.has_value());
    check(verdict->characters.empty());
};

auto tKeyVerdictKindIsCaseSensitive =
    test("WebViewMessages/keyVerdictKindIsCaseSensitive") = []
{
    check(!parseKeyVerdict(R"({"kind":"Down"})").has_value());
    check(!parseKeyVerdict(R"({"kind":""})").has_value());
};

auto tKeyVerdictRejectsNonObjectJson =
    test("WebViewMessages/keyVerdictRejectsNonObjectJson") = []
{
    check(!parseKeyVerdict("null").has_value());
    check(!parseKeyVerdict("42").has_value());
    check(!parseKeyVerdict("\"down\"").has_value());
};

auto tKeyVerdictWrongFieldTypesFallBackToDefaults =
    test("WebViewMessages/keyVerdictWrongFieldTypesFallBackToDefaults") = []
{
    auto verdict = parseKeyVerdict(
        R"({"kind":"down","keyCode":"65","consumed":"yes","modifiers":"1"})");
    check(verdict.has_value());
    check(verdict->virtualKey == 0);
    check(!verdict->consumed);
    check(!verdict->modifiers.shift);
    check(!parseKeyVerdict(R"({"kind":7})").has_value());
};

auto tKeyVerdictIgnoresUnknownFields =
    test("WebViewMessages/keyVerdictIgnoresUnknownFields") = []
{
    auto verdict = parseKeyVerdict(makeKeyReport(R"("keyCode":65,"extra":[1])"));
    check(verdict.has_value());
    check(verdict->virtualKey == letterAVirtualKey);
};

auto tEnvelopeMissingNameIsEmpty =
    test("WebViewMessages/envelopeMissingNameIsEmpty") = []
{
    auto message = parseWebMessageEnvelope(R"({"body":"hi"})");
    check(message.has_value());
    check(message->name.empty());
    check(message->body == "hi");
};

auto tEnvelopeNonStringNameIsEmpty =
    test("WebViewMessages/envelopeNonStringNameIsEmpty") = []
{
    auto message = parseWebMessageEnvelope(R"({"name":5,"body":"hi"})");
    check(message.has_value());
    check(message->name.empty());
};

auto tEnvelopeArrayBodyIsPrinted =
    test("WebViewMessages/envelopeArrayBodyIsPrinted") = []
{
    auto message = parseWebMessageEnvelope(R"({"name":"probe","body":[1,"two"]})");
    check(message.has_value());
    check(message->body.find("\"two\"") != std::string::npos);
    check(message->body.front() == '[');
};

auto tEnvelopeNullAndBoolBodiesAreEmpty =
    test("WebViewMessages/envelopeNullAndBoolBodiesAreEmpty") = []
{
    check(parseWebMessageEnvelope(R"({"name":"a","body":null})")->body.empty());
    check(parseWebMessageEnvelope(R"({"name":"a","body":true})")->body.empty());
    check(parseWebMessageEnvelope(R"({"name":"a"})")->body.empty());
};

auto tEnvelopeStringBodyKeepsEscapedText =
    test("WebViewMessages/envelopeStringBodyKeepsEscapedText") = []
{
    auto message =
        parseWebMessageEnvelope(R"({"name":"a","body":"{\"type\":\"ready\"}"})");
    check(message.has_value());
    check(message->body == R"({"type":"ready"})");
};

auto tScriptResultNullAndBoolPassThrough =
    test("WebViewMessages/scriptResultNullAndBoolPassThrough") = []
{
    check(decodeScriptResult("null") == "null");
    check(decodeScriptResult("true") == "true");
    check(decodeScriptResult("[1,2]") == "[1,2]");
    check(decodeScriptResult("").empty());
};

auto tScriptResultDecodesEscapesOneLayer =
    test("WebViewMessages/scriptResultDecodesEscapesOneLayer") = []
{
    check(decodeScriptResult(R"("")").empty());
    check(decodeScriptResult(R"("a\"b\\c\nd")") == "a\"b\\c\nd");
    check(decodeScriptResult(R"("é")") == "\xC3\xA9");
    check(decodeScriptResult(R"("\"inner\"")") == "\"inner\"");
};
