#include "Common.h"
// Windows-only: on macOS a DOM-dispatched event has no stashed NSEvent to pair
// the verdict with, so nothing would forward.
using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
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
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    // A genuine DOM KeyboardEvent, so key-events.js runs its full path.
    // `keyCode` is the DOM/Win32 virtual key value.
    void dispatchKey(const std::string& target,
                     const std::string& type,
                     const std::string& key,
                     int keyCode)
    {
        // cancelable: true so the page's preventDefault() actually takes.
        auto script = target + ".dispatchEvent(new KeyboardEvent('" + type
                      + "', {key: '" + key + "', keyCode: " + std::to_string(keyCode)
                      + ", bubbles: true, cancelable: true}))";
        webView.callJS(script + "; 'ok'").waitFor(webViewResultTimeout);
    }

    bool waitForReceivedCount(int count)
    {
        return Threads::runEventLoopUntil([this, count]
                                          { return received.size() >= count; },
                                          webViewResultTimeout);
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

    // Space is the sentinel that flushes the pipeline: verdicts arrive in
    // delivery order, so once Space is out the 'x' verdict was processed.
    fix.dispatchKey("window", "keydown", "x", 'X');
    fix.dispatchKey("window", "keydown", " ", 32);

    check(fix.waitForReceivedCount(1));
    check(fix.received.size() == 1);
    check(fix.received[0].keyCode == KeyCode::Space);
};

auto tKeyForwardEditable = test("KeyForwardingWin/typingInTextInputStaysInPage") = []
{
    auto fix = Fixture {};

    // key-events.js sees a text-field target and treats it as consumed.
    fix.dispatchKey("document.getElementById('field')", "keydown", "b", 'B');
    fix.dispatchKey("window", "keydown", " ", 32); // sentinel, forwarded

    check(fix.waitForReceivedCount(1));
    check(fix.received.size() == 1);
    check(fix.received[0].keyCode == KeyCode::Space);
};
