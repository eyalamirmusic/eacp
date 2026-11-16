#include "Common.h"
#include "DomKeyEvents.h"
#include <eacp/WebView/WebView/WebViewMessages.h>

// Drives key-events.js on a real WebView with DOM-dispatched KeyboardEvents and
// reads each report it posts through parseKeyVerdict: the page's verdict and
// the key's identity, on both backends. The page tees __eacpKeyEvent's
// postMessage to a test handler, so the reports are the exact objects the
// native side reads. No OS key injection: the events start in the page.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;
using namespace eacp::Graphics::detail;
using namespace DomKeyEvents;

namespace KeyVerdictTests
{
const std::string keyVerdictPageHtml = R"HTML(
<!doctype html>
<html>

<body>
  <input id="text" type="text" />
  <input id="range" type="range" />
  <input id="checkbox" type="checkbox" />
  <button id="button">button</button>
  <textarea id="area"></textarea>
  <div id="editable" contenteditable="true">editable</div>
  <select id="choice"><option>one</option></select>
  <div id="plain">plain</div>
  <script>
    window.addEventListener('keydown', (e) => { if (e.key === 'x') e.preventDefault(); });
    const keyHandler = window.webkit.messageHandlers.__eacpKeyEvent;
    const postToNative = keyHandler.postMessage.bind(keyHandler);
    keyHandler.postMessage = (report) => {
      window.webkit.messageHandlers.keyReport.postMessage(JSON.stringify(report));
      postToNative(report);
    };
    window.webkit.messageHandlers.ready.postMessage('ready');
  </script>
</body>

</html>
)HTML";

WebView::Options createKeyVerdictOptions()
{
    auto options = WebView::Options {};
    options.forwardUnhandledKeys = true;
    return options;
}

struct KeyVerdictFixture
{
    WebView webView {createKeyVerdictOptions()};
    Window window {};
    bool ready = false;
    Vector<KeyVerdict> verdicts;

    KeyVerdictFixture()
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.addScriptMessageHandler("keyReport",
                                        [this](const std::string& report)
                                        {
                                            if (auto verdict =
                                                    parseKeyVerdict(report))
                                                verdicts.add(*verdict);
                                        });
        webView.onUnhandledKeyEvent = [](const KeyEvent&) { return true; };
        webView.loadHTML(keyVerdictPageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    void dispatchKey(const DomKeyPress& press)
    {
        webView.callJS(createDomKeyDispatchScript(press))
            .waitFor(webViewResultTimeout);
    }

    KeyVerdict dispatchKeyAndReadVerdict(const DomKeyPress& press)
    {
        auto expectedCount = verdicts.size() + 1;
        dispatchKey(press);

        auto arrived = Threads::runEventLoopUntil(
            [this, expectedCount] { return verdicts.size() >= expectedCount; },
            webViewResultTimeout);
        check(arrived);

        if (!arrived)
            return {};

        return verdicts.back();
    }

    KeyVerdict dispatchKeyAt(const std::string& elementId,
                             const DomNamedKey& namedKey)
    {
        auto press = createDomKeyPress(namedKey);
        press.target = getElementExpression(elementId);
        return dispatchKeyAndReadVerdict(press);
    }
};

} // namespace KeyVerdictTests

using namespace KeyVerdictTests;

auto tKeyVerdictNavigationKeysPassOn =
    test("KeyVerdict/navigationKeysOnPlainContentArePassedOn") = []
{
    auto fix = KeyVerdictFixture {};

    for (auto& navigationKey: domNavigationKeys)
    {
        auto verdict = fix.dispatchKeyAt("plain", navigationKey);
        check(verdict.isDown);
        check(!verdict.consumed, navigationKey.key);
        check(verdict.virtualKey == navigationKey.keyCode, navigationKey.key);
        check(verdict.characters.empty(), navigationKey.key);
    }
};

auto tKeyVerdictSpacePassesOn = test("KeyVerdict/spaceOnPlainContentIsPassedOn") = []
{
    auto fix = KeyVerdictFixture {};

    auto verdict = fix.dispatchKeyAt("plain", spaceKey);
    check(!verdict.consumed);
    check(verdict.virtualKey == spaceVirtualKey);
    check(verdict.characters == " ");
};

auto tKeyVerdictShiftSpaceCarriesShift =
    test("KeyVerdict/shiftSpaceCarriesShift") = []
{
    auto fix = KeyVerdictFixture {};

    auto press = createDomKeyPress(spaceKey);
    press.shift = true;

    auto verdict = fix.dispatchKeyAndReadVerdict(press);
    check(!verdict.consumed);
    check(verdict.modifiers.shift);
    check(!verdict.modifiers.control);
    check(!verdict.modifiers.alt);
    check(!verdict.modifiers.command);
};

auto tKeyVerdictControlAndAltAreReported =
    test("KeyVerdict/controlAndAltAreReported") = []
{
    auto fix = KeyVerdictFixture {};

    auto press = createDomKeyPress(letterBKey);
    press.control = true;
    press.alt = true;

    auto verdict = fix.dispatchKeyAndReadVerdict(press);
    check(!verdict.modifiers.shift);
    check(verdict.modifiers.control);
    check(verdict.modifiers.alt);
};

auto tKeyVerdictCommandKeysAreNotReported =
    test("KeyVerdict/commandKeysAreNotReported") = []
{
    auto fix = KeyVerdictFixture {};

    auto commandPress = createDomKeyPress(letterBKey);
    commandPress.meta = true;
    fix.dispatchKey(commandPress);

    auto sentinel = fix.dispatchKeyAt("plain", spaceKey);
    check(fix.verdicts.size() == 1);
    check(sentinel.virtualKey == spaceVirtualKey);
};

auto tKeyVerdictRepeatIsReported = test("KeyVerdict/repeatIsReported") = []
{
    auto fix = KeyVerdictFixture {};

    auto press = createDomKeyPress(pageDownKey);
    press.repeat = true;

    check(fix.dispatchKeyAndReadVerdict(press).isRepeat);
};

auto tKeyVerdictKeyUpIsReported = test("KeyVerdict/keyUpIsReported") = []
{
    auto fix = KeyVerdictFixture {};

    auto press = createDomKeyPress({"Home", homeVirtualKey});
    press.type = "keyup";

    auto verdict = fix.dispatchKeyAndReadVerdict(press);
    check(!verdict.isDown);
    check(!verdict.consumed);
    check(verdict.virtualKey == homeVirtualKey);
};

auto tKeyVerdictTabStaysInPage = test("KeyVerdict/tabStaysInThePage") = []
{
    auto fix = KeyVerdictFixture {};
    check(fix.dispatchKeyAt("plain", tabKey).consumed);
};

auto tKeyVerdictPreventDefaultConsumes =
    test("KeyVerdict/preventDefaultConsumes") = []
{
    auto fix = KeyVerdictFixture {};
    check(fix.dispatchKeyAt("plain", letterXKey).consumed);
};

auto tKeyVerdictEditableTargetsConsume =
    test("KeyVerdict/editableTargetsConsumeEveryKey") = []
{
    auto fix = KeyVerdictFixture {};

    for (auto elementId: {"text", "area", "editable", "choice"})
    {
        check(fix.dispatchKeyAt(elementId, spaceKey).consumed, elementId);
        check(fix.dispatchKeyAt(elementId, pageDownKey).consumed, elementId);
    }
};

auto tKeyVerdictRangeInputConsumesNavigationOnly =
    test("KeyVerdict/rangeInputConsumesNavigationKeysOnly") = []
{
    auto fix = KeyVerdictFixture {};

    for (auto& navigationKey: domNavigationKeys)
    {
        auto verdict = fix.dispatchKeyAt("range", navigationKey);
        check(verdict.consumed, navigationKey.key);
    }

    check(!fix.dispatchKeyAt("range", spaceKey).consumed);
};

auto tKeyVerdictButtonsConsumeActivationOnly =
    test("KeyVerdict/buttonsConsumeActivationKeysOnly") = []
{
    auto fix = KeyVerdictFixture {};

    for (auto elementId: {"button", "checkbox"})
    {
        check(fix.dispatchKeyAt(elementId, spaceKey).consumed, elementId);
        check(!fix.dispatchKeyAt(elementId, pageUpKey).consumed, elementId);
    }
};

auto tKeyVerdictComposingKeyStaysInPage =
    test("KeyVerdict/composingKeyStaysInThePage") = []
{
    auto fix = KeyVerdictFixture {};

    auto press = createDomKeyPress(letterBKey);
    press.isComposing = true;

    check(fix.dispatchKeyAndReadVerdict(press).consumed);
};

auto tKeyVerdictDeadKeyHasNoCharacters =
    test("KeyVerdict/deadKeyIsPassedOnWithNoCharacters") = []
{
    auto fix = KeyVerdictFixture {};

    auto verdict = fix.dispatchKeyAt("plain", {"Dead", deadKeyVirtualKey});
    check(!verdict.consumed);
    check(verdict.virtualKey == deadKeyVirtualKey);
    check(verdict.characters.empty());
};

auto tKeyVerdictsArriveInDispatchOrder =
    test("KeyVerdict/verdictsArriveInDispatchOrder") = []
{
    auto fix = KeyVerdictFixture {};

    for (auto& navigationKey: domNavigationKeys)
    {
        fix.dispatchKey(createDomKeyPress(navigationKey));
    }

    check(Threads::runEventLoopUntil(
        [&fix] { return fix.verdicts.size() >= domNavigationKeys.size(); },
        webViewResultTimeout));

    for (auto index = 0; index < domNavigationKeys.size(); ++index)
        check(fix.verdicts[index].virtualKey == domNavigationKeys[index].keyCode);
};
