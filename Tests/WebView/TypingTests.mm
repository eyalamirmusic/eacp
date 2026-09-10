#include "Common.h"
// Text insertion end to end on a real WKWebView: a synthesized NSEvent goes
// to the platform view and the character has to land in the focused field.
// The KeyForwarding suite checks the page's consumed/unconsumed verdicts; this
// one checks the keystroke actually became text, which is what an app user
// sees. The NSEvent synthesis is AppKit-specific, so this suite is macOS-only.
//
// The second case exists because of a WebKit quirk that cost a release: an
// app-style stylesheet carrying `* { user-select: none }` reaches text fields
// too, and WebKit on macOS 12 honours that inside the field — the editor can
// never place a selection, so keydown/keypress fire and nothing is inserted.
// Newer WebKit ignores user-select for editable content, so the same page
// types fine there. The case pins the behaviour a page can rely on.

#import <AppKit/AppKit.h>

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
std::string pageHtml(const std::string& extraCss)
{
    return R"HTML(
<!doctype html>
<html>
<head><style>)HTML"
           + extraCss + R"HTML(</style></head>
<body>
  <input id="field" type="text" />
  <script>window.webkit.messageHandlers.ready.postMessage('ready');</script>
</body>
</html>
)HTML";
}

struct Fixture
{
    WebView webView {};
    Window window {};
    bool ready = false;

    explicit Fixture(const std::string& extraCss)
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler("ready",
                                        [this](const std::string&)
                                        { ready = true; });
        webView.loadHTML(pageHtml(extraCss));
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    // WebKit only honours in-page focus() while the hosting window is key,
    // so the field needs the window focused for real. Not achievable in every
    // environment (headless CI can't activate), hence a bool rather than a
    // check — see KeyForwardingTests for the same arrangement.
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

    // Straight to the window's first responder — the platform web view —
    // which is exactly where a real key press lands.
    void typeKey(uint16_t keyCode, NSString* characters)
    {
        auto* nsWindow = (NSWindow*) window.getHandle();
        auto* responder = nsWindow.firstResponder;

        for (auto type: {NSEventTypeKeyDown, NSEventTypeKeyUp})
        {
            auto* event = [NSEvent keyEventWithType:type
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
        }
    }

    std::string runJS(const std::string& script)
    {
        return webView.callJS(script).waitFor(webViewResultTimeout);
    }

    bool waitForFieldValue(const std::string& expected)
    {
        return Threads::runEventLoopUntil(
            [this, &expected]
            { return runJS("document.getElementById('field').value") == expected; },
            webViewResultTimeout);
    }
};

void checkTypingInserts(Fixture& fix)
{
    if (!fix.makeWindowKey())
        return; // no window focus to be had here; nothing to assert

    fix.runJS("document.getElementById('field').focus()");
    fix.typeKey(KeyCode::A, @"a");
    fix.typeKey(KeyCode::B, @"b");

    check(fix.waitForFieldValue("ab"));
}
} // namespace

auto tTypingInsertsText = test("Typing/keyDownInsertsText") = []
{
    auto fix = Fixture {""};
    checkTypingInserts(fix);
};

auto tTypingUnderUserSelectNone =
    test("Typing/insertsTextUnderUniversalUserSelectNone") = []
{
    auto fix = Fixture {"* { -webkit-user-select: none; user-select: none; }"};
    checkTypingInserts(fix);
};
