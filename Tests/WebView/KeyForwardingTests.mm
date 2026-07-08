// Drives the unhandled-key forwarding pipeline (Options::forwardUnhandledKeys)
// end to end on a real WKWebView: a synthesized NSEvent goes to the platform
// view, the injected key-events.js reports whether the page consumed it, and
// the unconsumed ones must come back out through onUnhandledKeyEvent. The
// NSEvent synthesis is AppKit-specific, so this suite is macOS-only.

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/WebView/WebView.h>

#import <AppKit/AppKit.h>

#include <NanoTest/NanoTest.h>

#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;
using namespace std::chrono_literals;

namespace
{
// A text input to test implicit consumption, plus a page handler that
// explicitly consumes the 'a' key (and only that key) with preventDefault.
// `ready` gates the tests until document-start injection has run.
const std::string pageHtml = R"HTML(
<!doctype html>
<html>

<body>
  <input id="field" type="text" />
  <script>
    window.addEventListener('keydown', (e) => { if (e.key === 'a') e.preventDefault(); });
    window.addEventListener('keyup', (e) => { if (e.key === 'a') e.preventDefault(); });
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
        webView.addScriptMessageHandler("ready",
                                        [this](const std::string&)
                                        { ready = true; });
        webView.onUnhandledKeyEvent = [this](const KeyEvent& event)
        {
            received.add(event);
            return true;
        };
        webView.loadHTML(pageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; }, 10s));
        webView.focusContent();
    }

    // WebKit only honours in-page focus() (and reports the real event target)
    // while the hosting window is key, so the editable-target test needs the
    // window focused for real. Not achievable in every environment (headless
    // CI can't activate), hence a bool rather than a check.
    bool makeWindowKey()
    {
        window.toFront();

        auto* nsWindow = (NSWindow*) window.getHandle();
        auto isKey = Threads::runEventLoopUntil(
            [nsWindow] { return nsWindow.keyWindow; }, 2s);

        if (isKey)
            webView.focusContent();

        return isKey;
    }

    // Synthesizes a down+up pair straight to the window's first responder --
    // the platform web view -- which is exactly where a real key press lands.
    void sendKey(uint16_t keyCode, NSString* characters)
    {
        auto* nsWindow = (NSWindow*) window.getHandle();
        auto* responder = nsWindow.firstResponder;

        auto sendEvent = [&](NSEventType type)
        {
            auto* event =
                [NSEvent keyEventWithType:type
                                 location:NSZeroPoint
                            modifierFlags:0
                                timestamp:[NSProcessInfo processInfo].systemUptime
                             windowNumber:nsWindow.windowNumber
                                  context:nil
                               characters:characters
              charactersIgnoringModifiers:characters
                                isARepeat:NO
                                  keyCode:keyCode];

            if (type == NSEventTypeKeyDown)
                [responder keyDown:event];
            else
                [responder keyUp:event];
        };

        sendEvent(NSEventTypeKeyDown);
        sendEvent(NSEventTypeKeyUp);
    }

    bool waitForReceivedCount(int count)
    {
        return Threads::runEventLoopUntil(
            [this, count] { return received.size() >= count; }, 10s);
    }

    void runJS(const std::string& script)
    {
        webView.callJS(script + "; 'ok'").waitFor(10s);
    }
};
} // namespace

auto tKeyForwardUnhandled = test("KeyForwarding/unhandledKeyReachesCallback") = []
{
    auto fix = Fixture {};

    fix.sendKey(KeyCode::Space, @" ");

    check(fix.waitForReceivedCount(2));
    check(fix.received[0].type == KeyEventType::Down);
    check(fix.received[0].keyCode == KeyCode::Space);
    check(fix.received[1].type == KeyEventType::Up);
    check(fix.received[1].keyCode == KeyCode::Space);
};

auto tKeyForwardPreventDefault = test("KeyForwarding/preventDefaultConsumes") = []
{
    auto fix = Fixture {};

    // The page preventDefaults 'a'; Space is the sentinel that flushes the
    // pipeline -- verdicts arrive in delivery order, so once Space is out,
    // the 'a' verdicts have already been processed.
    fix.sendKey(KeyCode::A, @"a");
    fix.sendKey(KeyCode::Space, @" ");

    check(fix.waitForReceivedCount(2));
    check(fix.received.size() == 2);
    check(fix.received[0].keyCode == KeyCode::Space);
    check(fix.received[1].keyCode == KeyCode::Space);
};

auto tKeyForwardEditable = test("KeyForwarding/typingInTextInputStaysInPage") = []
{
    auto fix = Fixture {};

    if (!fix.makeWindowKey())
        return; // no window focus to be had here; nothing to assert

    fix.runJS("document.getElementById('field').focus()");
    fix.sendKey(KeyCode::B, @"b"); // lands on the input: implicitly consumed

    fix.runJS("document.getElementById('field').blur()");
    fix.sendKey(KeyCode::Space, @" "); // sentinel, and forwarded

    check(fix.waitForReceivedCount(2));
    check(fix.received.size() == 2);
    check(fix.received[0].keyCode == KeyCode::Space);
    check(fix.received[1].keyCode == KeyCode::Space);
};
