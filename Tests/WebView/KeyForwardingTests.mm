#include "Common.h"
// Drives the unhandled-key forwarding pipeline (Options::forwardUnhandledKeys)
// end to end on a real WKWebView: a synthesized NSEvent goes to the platform
// view, the injected key-events.js reports whether the page consumed it, and
// the unconsumed ones must come back out through onUnhandledKeyEvent. The
// NSEvent synthesis is AppKit-specific, so this suite is macOS-only.

#import <AppKit/AppKit.h>

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

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
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
        webView.focusContent();
    }

    // WebKit only honours in-page focus() (and reports the real event target)
    // while the hosting window is key, so the editable-target test needs the
    // window focused for real. Not achievable in every environment (headless
    // CI can't activate), hence a bool rather than a check.
    //
    // The wait is short because activation is effectively binary: an active
    // app gets a key window within a turn or two of the loop, and an app that
    // cannot activate at all never will, however long it is given. Where it
    // cannot, this wait is the whole cost of the test.
    bool makeWindowKey()
    {
        window.toFront();

        auto* nsWindow = (NSWindow*) window.getHandle();
        auto isKey = Threads::runEventLoopUntil(
            [nsWindow] { return nsWindow.keyWindow; }, eacp::Time::MS {150});

        if (isKey)
            webView.focusContent();

        return isKey;
    }

    // Straight to the window's first responder -- the platform web view --
    // which is exactly where a real key press lands. The timestamp is explicit
    // because it is the identity AppKit stamps on an event at creation, and the
    // echo test needs to re-send one that carries an identity already seen.
    void dispatchKey(NSEventType type,
                     uint16_t keyCode,
                     NSString* characters,
                     NSTimeInterval timestamp)
    {
        auto* nsWindow = (NSWindow*) window.getHandle();
        auto* responder = nsWindow.firstResponder;

        auto* event = [NSEvent keyEventWithType:type
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:timestamp
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
    }

    void sendKey(uint16_t keyCode, NSString* characters)
    {
        auto now = [] { return [NSProcessInfo processInfo].systemUptime; };

        dispatchKey(NSEventTypeKeyDown, keyCode, characters, now());
        dispatchKey(NSEventTypeKeyUp, keyCode, characters, now());
    }

    bool waitForReceivedCount(int count)
    {
        return Threads::runEventLoopUntil(
            [this, count] { return received.size() >= count; }, webViewResultTimeout);
    }

    bool waitForKeyUp(uint16_t keyCode)
    {
        return Threads::runEventLoopUntil(
            [this, keyCode]
            {
                for (auto& event: received)
                {
                    if (event.type == KeyEventType::Up && event.keyCode == keyCode)
                        return true;
                }

                return false;
            },
            webViewResultTimeout);
    }

    void runJS(const std::string& script)
    {
        webView.callJS(script + "; 'ok'").waitFor(webViewResultTimeout);
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

// An out-of-process host (Logic runs AU editors in AUHostingServiceXPC)
// dispatches a key we handed to its responder chain straight back into this
// view. It arrives re-encoded across the boundary -- a different NSEvent
// object, isARepeat NO -- so only the timestamp AppKit stamped on the original
// identifies it as one we have already reported. Report it again and it is
// forwarded again: one keypress becomes an unbounded round trip that freezes
// the host. Here the test plays the host, bouncing the pair back.
auto tKeyForwardHostEcho = test("KeyForwarding/hostEchoIsReportedOnce") = []
{
    auto fix = Fixture {};

    fix.sendKey(KeyCode::Space, @" ");
    check(fix.waitForReceivedCount(2));

    fix.dispatchKey(NSEventTypeKeyDown,
                    KeyCode::Space,
                    @" ",
                    fix.received[0].timestamp);
    fix.dispatchKey(NSEventTypeKeyUp,
                    KeyCode::Space,
                    @" ",
                    fix.received[1].timestamp);

    // Sentinel: verdicts arrive in delivery order, so once 'b' is out the
    // echoed pair has already been through the pipeline.
    fix.sendKey(KeyCode::B, @"b");

    check(fix.waitForKeyUp(KeyCode::B));
    check(fix.received.size() == 4);
    check(fix.received[2].keyCode == KeyCode::B);
    check(fix.received[3].keyCode == KeyCode::B);
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
