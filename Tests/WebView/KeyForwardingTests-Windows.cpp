#include "Common.h"
#include "DomKeyEvents.h"
// Drives the unhandled-key forwarding pipeline (Options::forwardUnhandledKeys)
// end to end on a real WebView2: a page dispatches DOM KeyboardEvents, the
// injected key-events.js reports whether the page consumed each one plus its
// identity, and the unconsumed ones must come back out through
// onUnhandledKeyEvent. Unlike the macOS suite (KeyForwardingTests.mm), the
// Windows backend keeps no native event stash -- the verdict message carries
// the key identity -- so dispatching synthetic DOM events exercises the whole of
// our code without OS-level key injection. That same reason makes this a
// Windows-only suite: on macOS a DOM-dispatched event has no stashed NSEvent to
// pair the verdict with, so nothing would forward.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;
using namespace DomKeyEvents;

namespace
{
// A text input to test implicit consumption, plus a handler that explicitly
// consumes the 'x' key (and only that key) with preventDefault. `ready` gates
// the tests until document-start injection has run.
const std::string keyForwardingPageHtml = R"HTML(
<!doctype html>
<html>

<body>
  <input id="field" type="text" />
  <textarea id="area"></textarea>
  <div id="editable" contenteditable="true">editable</div>
  <script>
    window.addEventListener('keydown', (e) => { if (e.key === 'x') e.preventDefault(); });
    window.webkit.messageHandlers.ready.postMessage('ready');
  </script>
</body>

</html>
)HTML";

WebView::Options forwardingOptions()
{
    auto options = WebView::Options {};
    options.forwardUnhandledKeys = true;
    return options;
}

struct HostKeyRecorder : WindowInputListener
{
    Vector<KeyEvent> received;

    void windowKeyEvent(const KeyEvent& event) override { received.add(event); }
};

struct KeyForwardingFixture
{
    WebView webView {forwardingOptions()};
    Window window {};
    bool ready = false;
    bool claimsUnhandledKeys = true;
    Vector<KeyEvent> received;
    HostKeyRecorder hostKeys;

    KeyForwardingFixture()
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        window.events.input.addListener(hostKeys);
        webView.onUnhandledKeyEvent = [this](const KeyEvent& event)
        {
            received.add(event);
            return claimsUnhandledKeys;
        };
        webView.loadHTML(keyForwardingPageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    ~KeyForwardingFixture() { window.events.input.removeListener(hostKeys); }

    void dispatchKey(const DomKeyPress& press)
    {
        webView.callJS(createDomKeyDispatchScript(press))
            .waitFor(webViewResultTimeout);
    }

    void dispatchKeyDownAt(const std::string& target, const DomNamedKey& namedKey)
    {
        auto press = createDomKeyPress(namedKey);
        press.target = target;
        dispatchKey(press);
    }

    void dispatchSentinelSpace() { dispatchKey(createDomKeyPress(spaceKey)); }

    bool waitForReceivedCount(int count)
    {
        return Threads::runEventLoopUntil([this, count]
                                          { return received.size() >= count; },
                                          webViewResultTimeout);
    }

    bool hasOnlyTheSentinelArrived()
    {
        return waitForReceivedCount(1) && received.size() == 1
               && received[0].keyCode == KeyCode::Space;
    }
};

struct ExpectedKeyCode
{
    DomNamedKey domKey;
    uint16_t keyCode = 0;
};

const Vector<ExpectedKeyCode> navigationKeyCodes = {
    {domNavigationKeys[0], KeyCode::PageUp},
    {domNavigationKeys[1], KeyCode::PageDown},
    {domNavigationKeys[2], KeyCode::Home},
    {domNavigationKeys[3], KeyCode::End},
    {domNavigationKeys[4], KeyCode::LeftArrow},
    {domNavigationKeys[5], KeyCode::UpArrow},
    {domNavigationKeys[6], KeyCode::RightArrow},
    {domNavigationKeys[7], KeyCode::DownArrow}};
} // namespace

auto tKeyForwardUnhandled = test("KeyForwardingWin/unhandledKeyReachesCallback") = []
{
    auto fix = KeyForwardingFixture {};

    fix.dispatchSentinelSpace();

    check(fix.waitForReceivedCount(1));
    check(fix.received[0].type == KeyEventType::Down);
    check(fix.received[0].keyCode == KeyCode::Space);
    check(fix.received[0].characters == " ");
};

auto tKeyForwardPreventDefault = test("KeyForwardingWin/preventDefaultConsumes") = []
{
    auto fix = KeyForwardingFixture {};

    // The page preventDefaults 'x'; Space is the sentinel that flushes the
    // pipeline -- verdicts arrive in delivery order, so once Space is out, the
    // 'x' verdict has already been processed (and dropped as consumed).
    fix.dispatchKey(createDomKeyPress(letterXKey));
    fix.dispatchSentinelSpace();

    check(fix.waitForReceivedCount(1));
    check(fix.received.size() == 1);
    check(fix.received[0].keyCode == KeyCode::Space);
};

auto tKeyForwardEditable = test("KeyForwardingWin/typingInTextInputStaysInPage") = []
{
    auto fix = KeyForwardingFixture {};

    // Dispatched at the input, so key-events.js sees a text-field target and
    // treats it as implicitly consumed -- it must not forward.
    fix.dispatchKeyDownAt(getElementExpression("field"), letterBKey);
    fix.dispatchSentinelSpace();

    check(fix.waitForReceivedCount(1));
    check(fix.received.size() == 1);
    check(fix.received[0].keyCode == KeyCode::Space);
};

auto tKeyForwardNavigationKeyCodes =
    test("KeyForwardingWin/navigationKeysReachCallbackWithTheirKeyCode") = []
{
    auto fix = KeyForwardingFixture {};

    for (auto& expected: navigationKeyCodes)
        fix.dispatchKey(createDomKeyPress(expected.domKey));

    if (!fix.waitForReceivedCount(navigationKeyCodes.size()))
    {
        check(false, "every navigation key is forwarded");
        return;
    }

    for (auto index = 0; index < navigationKeyCodes.size(); ++index)
    {
        check(fix.received[index].keyCode == navigationKeyCodes[index].keyCode,
              navigationKeyCodes[index].domKey.key);
        check(fix.received[index].characters.empty());
    }
};

auto tKeyForwardShiftSpace = test("KeyForwardingWin/shiftSpaceKeepsShift") = []
{
    auto fix = KeyForwardingFixture {};

    auto press = createDomKeyPress(spaceKey);
    press.shift = true;
    fix.dispatchKey(press);

    check(fix.hasOnlyTheSentinelArrived());
    check(!fix.received.empty() && fix.received[0].modifiers.shift);
    check(!fix.received.empty() && !fix.received[0].modifiers.control);
};

auto tKeyForwardRepeatAndKeyUp = test("KeyForwardingWin/repeatAndKeyUpArrive") = []
{
    auto fix = KeyForwardingFixture {};

    auto repeated = createDomKeyPress(pageDownKey);
    repeated.repeat = true;
    fix.dispatchKey(repeated);
    auto keyUp = createDomKeyPress(pageDownKey);
    keyUp.type = "keyup";
    fix.dispatchKey(keyUp);

    if (!fix.waitForReceivedCount(2))
    {
        check(false, "the repeat and the key-up are forwarded");
        return;
    }

    check(fix.received[0].type == KeyEventType::Down);
    check(fix.received[0].isRepeat);
    check(fix.received[1].type == KeyEventType::Up);
    check(fix.received[1].keyCode == KeyCode::PageDown);
};

auto tKeyForwardTabStaysInPage = test("KeyForwardingWin/tabStaysInThePage") = []
{
    auto fix = KeyForwardingFixture {};

    fix.dispatchKey(createDomKeyPress(tabKey));
    fix.dispatchSentinelSpace();

    check(fix.hasOnlyTheSentinelArrived());
};

auto tKeyForwardEditableTargets =
    test("KeyForwardingWin/textareaAndContentEditableStayInPage") = []
{
    auto fix = KeyForwardingFixture {};

    fix.dispatchKeyDownAt(getElementExpression("area"), spaceKey);
    fix.dispatchKeyDownAt(getElementExpression("editable"), pageDownKey);
    fix.dispatchSentinelSpace();

    check(fix.hasOnlyTheSentinelArrived());
};

auto tKeyForwardMalformedDropped =
    test("KeyForwardingWin/malformedAndUnknownKindMessagesAreDropped") = []
{
    auto fix = KeyForwardingFixture {};

    fix.webView
        .callJS(
            "{const keyHandler = window.webkit.messageHandlers.__eacpKeyEvent;"
            "keyHandler.postMessage('down:0:66:0:0:b');"
            "keyHandler.postMessage('{not json');"
            "keyHandler.postMessage([66]);"
            "keyHandler.postMessage({kind: 'press', consumed: false, keyCode: 66})}; 'ok'")
        .waitFor(webViewResultTimeout);
    fix.dispatchSentinelSpace();

    check(fix.hasOnlyTheSentinelArrived());
};

auto tKeyForwardReachesHostWindow =
    test("KeyForwardingWin/unclaimedKeyReachesTheHostWindow") = []
{
    auto fix = KeyForwardingFixture {};
    fix.claimsUnhandledKeys = false;

    fix.dispatchKey(createDomKeyPress(pageDownKey));

    auto arrived = Threads::runEventLoopUntil(
        [&fix] { return !fix.hostKeys.received.empty(); }, webViewResultTimeout);
    check(arrived);

    if (!arrived)
        return;

    check(fix.hostKeys.received[0].type == KeyEventType::Down);
    check(fix.hostKeys.received[0].keyCode == KeyCode::PageDown);
};
