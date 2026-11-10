#include "Common.h"

// The framework's routed mouse events forwarded into a real WebView2 through
// its composition controller. The test calls the View's mouse handlers the way
// the host window does; nothing is injected into the OS.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace WebViewMouseTests
{
const auto clickPosition = Point {50, 60};
const auto dragEndPosition = Point {120, 90};
const auto viewBounds = Rect {0, 0, 400, 300};
constexpr auto wheelLinesTowardTheEnd = -3.0f;
constexpr auto leftButtonMask = 1;

const std::string mousePageHtml = R"HTML(<!doctype html><html>
<body style="margin:0; height:4000px"><script>
function report(event) {
  window.webkit.messageHandlers.mouse.postMessage(JSON.stringify({
    type: event.type, x: event.clientX, y: event.clientY, buttons: event.buttons,
  }));
}
for (const type of ['mousedown', 'mouseup', 'click'])
  window.addEventListener(type, report);
window.addEventListener('mousemove', (event) => { if (event.buttons) report(event); });
window.webkit.messageHandlers.ready.postMessage('ready');
</script></body></html>)HTML";

struct PageMouseEvent
{
    std::string type;
    int x = 0;
    int y = 0;
    int buttons = 0;

    MIRO_REFLECT(type, x, y, buttons)
};

MouseEvent createMouseEvent(MouseEventType type, Point position)
{
    auto event = MouseEvent {};
    event.type = type;
    event.pos = position;
    event.downPos = clickPosition;
    return event;
}

struct MouseFixture
{
    View container;
    WebView webView {};
    Window window {};
    bool ready = false;
    Vector<PageMouseEvent> received;

    MouseFixture()
    {
        container.addSubview(webView);
        webView.setBounds(viewBounds);
        window.setContentView(container);

        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.addScriptMessageHandler("mouse",
                                        [this](const std::string& json)
                                        {
                                            auto event = PageMouseEvent {};
                                            Miro::fromJSONString(event, json);
                                            received.add(event);
                                        });
        webView.loadHTML(mousePageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    View& getRoutedView() { return webView; }

    bool waitForEventOfType(const std::string& type)
    {
        return Threads::runEventLoopUntil(
            [this, type]
            {
                for (auto& event: received)
                {
                    if (event.type == type)
                        return true;
                }

                return false;
            },
            webViewResultTimeout);
    }

    std::optional<PageMouseEvent> findEventOfType(const std::string& type)
    {
        for (auto& event: received)
        {
            if (event.type == type)
                return event;
        }

        return std::nullopt;
    }
};
} // namespace WebViewMouseTests

using namespace WebViewMouseTests;

auto tMouseClickReachesPage = test("WebViewMouse/clickReachesThePageAtItsPoint") = []
{
    auto fix = MouseFixture {};

    fix.getRoutedView().mouseDown(
        createMouseEvent(MouseEventType::Down, clickPosition));
    fix.getRoutedView().mouseUp(createMouseEvent(MouseEventType::Up, clickPosition));

    check(fix.waitForEventOfType("click"));

    auto down = fix.findEventOfType("mousedown");
    check(down.has_value());
    check(down && down->x == static_cast<int>(clickPosition.x));
    check(down && down->y == static_cast<int>(clickPosition.y));
};

auto tMouseDragKeepsButtonHeld = test("WebViewMouse/dragKeepsTheButtonHeld") = []
{
    auto fix = MouseFixture {};

    fix.getRoutedView().mouseDown(
        createMouseEvent(MouseEventType::Down, clickPosition));
    fix.getRoutedView().mouseDragged(
        createMouseEvent(MouseEventType::Dragged, dragEndPosition));

    check(fix.waitForEventOfType("mousemove"));

    auto move = fix.findEventOfType("mousemove");
    check(move && move->buttons == leftButtonMask);
    check(move && move->x == static_cast<int>(dragEndPosition.x));

    fix.getRoutedView().mouseUp(
        createMouseEvent(MouseEventType::Up, dragEndPosition));
    check(fix.waitForEventOfType("mouseup"));
};

auto tMouseWheelScrollsPage = test("WebViewMouse/wheelScrollsThePage") = []
{
    auto fix = MouseFixture {};

    auto wheel = createMouseEvent(MouseEventType::Wheel, clickPosition);
    wheel.delta = Point {0, wheelLinesTowardTheEnd};
    fix.getRoutedView().mouseWheel(wheel);

    check(Threads::runEventLoopUntil(
        [&fix]
        {
            return fix.webView.callJS("String(window.scrollY > 0)")
                       .waitFor(webViewResultTimeout)
                   == "true";
        },
        webViewResultTimeout));
};
