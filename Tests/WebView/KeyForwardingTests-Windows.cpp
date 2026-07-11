#include "Common.h"
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
using namespace std::chrono_literals;

namespace
{
// A text input to test implicit consumption, plus a handler that explicitly
// consumes the 'x' key (and only that key) with preventDefault. `ready` gates
// the tests until document-start injection has run.
const std::string pageHtml = R"HTML(
<!doctype html>
<html>

<body>
  <input id="field" type="text" />
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

struct Fixture
{
    WebView webView {forwardingOptions()};
    Window window {};
    bool ready = false;
    Vector<KeyEvent> received;

    Fixture()
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.onUnhandledKeyEvent = [this](const KeyEvent& event)
        {
            received.add(event);
            return true;
        };
        webView.loadHTML(pageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; }, 10s));
    }

    // Dispatches a genuine DOM KeyboardEvent so the injected key-events.js runs
    // its full path (consumption check + identity-carrying verdict postMessage)
    // exactly as a real WebView2 key press would, minus the OS routing that is
    // not our code. `keyCode` is the DOM/Win32 virtual key value.
    void dispatchKey(const std::string& target,
                     const std::string& type,
                     const std::string& key,
                     int keyCode)
    {
        // cancelable: true so the page's preventDefault() actually takes, the
        // way a real key event (which is always cancelable) behaves.
        auto script = target + ".dispatchEvent(new KeyboardEvent('" + type
                      + "', {key: '" + key + "', keyCode: " + std::to_string(keyCode)
                      + ", bubbles: true, cancelable: true}))";
        webView.callJS(script + "; 'ok'").waitFor(10s);
    }

    bool waitForReceivedCount(int count)
    {
        return Threads::runEventLoopUntil(
            [this, count] { return received.size() >= count; }, 10s);
    }
};
} // namespace

auto tKeyForwardUnhandled = test("KeyForwardingWin/unhandledKeyReachesCallback") = []
{
    auto fix = Fixture {};

    fix.dispatchKey("window", "keydown", " ", 32);

    check(fix.waitForReceivedCount(1));
    check(fix.received[0].type == KeyEventType::Down);
    check(fix.received[0].keyCode == KeyCode::Space);
    check(fix.received[0].characters == " ");
};

auto tKeyForwardPreventDefault = test("KeyForwardingWin/preventDefaultConsumes") = []
{
    auto fix = Fixture {};

    // The page preventDefaults 'x'; Space is the sentinel that flushes the
    // pipeline -- verdicts arrive in delivery order, so once Space is out, the
    // 'x' verdict has already been processed (and dropped as consumed).
    fix.dispatchKey("window", "keydown", "x", 'X');
    fix.dispatchKey("window", "keydown", " ", 32);

    check(fix.waitForReceivedCount(1));
    check(fix.received.size() == 1);
    check(fix.received[0].keyCode == KeyCode::Space);
};

auto tKeyForwardEditable = test("KeyForwardingWin/typingInTextInputStaysInPage") = []
{
    auto fix = Fixture {};

    // Dispatched at the input, so key-events.js sees a text-field target and
    // treats it as implicitly consumed -- it must not forward.
    fix.dispatchKey("document.getElementById('field')", "keydown", "b", 'B');
    fix.dispatchKey("window", "keydown", " ", 32); // sentinel, forwarded

    check(fix.waitForReceivedCount(1));
    check(fix.received.size() == 1);
    check(fix.received[0].keyCode == KeyCode::Space);
};
