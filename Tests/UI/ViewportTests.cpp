#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>

// A viewport scrolled by the wheel, its scrollbars and a finger. Nothing is
// rendered and no display link runs: every event carries its own time and the
// host's animations are stepped by hand, so a fling is the same numbers on
// every run.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace ViewportTests
{
namespace
{
constexpr auto frame = 1.0 / 60.0;

eacp::Graphics::MouseEvent pointerAt(Point position, double time, bool fromTouch)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;
    event.timestamp = time;
    event.fromTouch = fromTouch;

    return event;
}

eacp::Graphics::MouseEvent mouseAt(Point position, double time = 0.0)
{
    return pointerAt(position, time, false);
}

eacp::Graphics::MouseEvent touchAt(Point position, double time)
{
    return pointerAt(position, time, true);
}

eacp::Graphics::MouseEvent wheel(Point delta, bool precise)
{
    auto event = mouseAt({100.f, 50.f});

    event.delta = delta;
    event.preciseScrolling = precise;

    return event;
}

bool near(float a, float b)
{
    return std::abs(a - b) < 0.01f;
}

bool samePoint(Point a, Point b)
{
    return a.x == b.x && a.y == b.y;
}

struct Content final : Component
{
    Content() { setInterceptsMouseClicks(true); }

    void mouseDown(const MouseEvent&) override { ++presses; }

    int presses = 0;
};

// A 200 by 100 viewport over 600 by 400 of content, so both bars show and the
// view is 192 by 92 inside them.
struct Harness
{
    explicit Harness(Rect contentBounds = {0.f, 0.f, 600.f, 400.f})
    {
        host.setAnimationClockEnabled(false);
        host.setBounds({0.f, 0.f, 200.f, 100.f});
        host.setRootComponent(root);

        root.addAndMakeVisible(viewport);
        viewport.setBounds({0.f, 0.f, 200.f, 100.f});

        content.setBounds(contentBounds);
        viewport.setContent(content);

        viewport.onViewPositionChanged = [this](Point position)
        {
            lastReported = position;
            ++changes;
        };
    }

    void swipe(Point from, Point to, int steps, double seconds)
    {
        host.mouseDown(touchAt(from, time));

        for (auto step = 1; step <= steps; ++step)
        {
            auto t = (float) step / (float) steps;
            time += seconds;
            host.mouseDragged(touchAt(
                {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t}, time));
        }

        host.mouseUp(touchAt(to, time));
    }

    int runFling(int maximumFrames = 2000)
    {
        auto frames = 0;

        while (host.isAnimating() && frames < maximumFrames)
        {
            host.advanceAnimations(frame);
            ++frames;
        }

        return frames;
    }

    ComponentHost host;
    Component root;
    Viewport viewport;
    Content content;

    Point lastReported;
    int changes = 0;
    double time = 10.0;
};
} // namespace

auto tWheelVertical = test("Viewport/theWheelScrollsDownAndIsClamped") = []
{
    auto harness = Harness {};
    auto& viewport = harness.viewport;

    check(samePoint(viewport.getMaximumViewPosition(), {408.f, 308.f}));
    check(sameRect(viewport.getViewArea(), {0.f, 0.f, 192.f, 92.f}),
          "both bars take their strip");

    harness.host.mouseWheel(wheel({0.f, -50.f}, true));
    check(samePoint(viewport.getViewPosition(), {0.f, 50.f}), "points as they come");

    harness.host.mouseWheel(wheel({0.f, -2.f}, false));
    check(samePoint(viewport.getViewPosition(), {0.f, 130.f}), "two lines of forty");

    harness.host.mouseWheel(wheel({0.f, -10000.f}, true));
    check(samePoint(viewport.getViewPosition(), {0.f, 308.f}), "stopped at the end");
    check(harness.content.getBounds().y == -308.f, "the content moved with it");

    harness.host.mouseWheel(wheel({0.f, 10000.f}, true));
    check(samePoint(viewport.getViewPosition(), {0.f, 0.f}), "and at the start");
};

auto tWheelHorizontal = test("Viewport/theWheelScrollsAcrossAndIsClamped") = []
{
    auto harness = Harness {};
    auto& viewport = harness.viewport;

    harness.host.mouseWheel(wheel({-30.f, 0.f}, true));
    check(samePoint(viewport.getViewPosition(), {30.f, 0.f}));

    harness.host.mouseWheel(wheel({-1.f, 0.f}, false));
    check(samePoint(viewport.getViewPosition(), {70.f, 0.f}));

    harness.host.mouseWheel(wheel({-10000.f, -10000.f}, true));
    check(samePoint(viewport.getViewPosition(), {408.f, 308.f}), "both at once");
    check(harness.content.getBounds().x == -408.f);

    auto shifted = wheel({0.f, 100.f}, true);
    shifted.modifiers.shift = true;
    harness.host.mouseWheel(shifted);
    check(samePoint(viewport.getViewPosition(), {308.f, 308.f}),
          "shift turns a vertical wheel across");
};

auto tWheelAcrossOnly = test("Viewport/aWheelOverWideContentScrollsAcross") = []
{
    auto harness = Harness {{0.f, 0.f, 600.f, 80.f}};
    auto& viewport = harness.viewport;

    check(viewport.getMaximumViewPosition().y == 0.f);

    harness.host.mouseWheel(wheel({0.f, -40.f}, true));
    check(samePoint(viewport.getViewPosition(), {40.f, 0.f}),
          "across is the only way to go, so the wheel goes that way");
};

auto tCallback = test("Viewport/aMoveIsReportedOnceAndClamped") = []
{
    auto harness = Harness {};
    auto& viewport = harness.viewport;

    viewport.setViewPosition({10.f, 20.f});
    check(harness.changes == 1);
    check(samePoint(harness.lastReported, {10.f, 20.f}));

    viewport.setViewPosition({10.f, 20.f});
    check(harness.changes == 1, "a position that did not move is not a change");

    viewport.setViewPosition({1000.f, -50.f});
    check(harness.changes == 2);
    check(samePoint(harness.lastReported, {408.f, 0.f}), "reported clamped");
    check(samePoint(viewport.getViewPosition(), {408.f, 0.f}));
};

auto tFits = test("Viewport/contentThatFitsDoesNotScrollAndLeavesTheWheel") = []
{
    struct WheelCatcher final : Component
    {
        WheelCatcher() { setInterceptsMouseClicks(true); }

        bool mouseWheelMove(const MouseEvent&) override
        {
            ++wheels;
            return true;
        }

        int wheels = 0;
    };

    auto host = ComponentHost {};
    auto parent = WheelCatcher {};
    auto viewport = Viewport {};
    auto content = Component {};

    host.setBounds({0.f, 0.f, 200.f, 100.f});
    host.setRootComponent(parent);
    parent.addAndMakeVisible(viewport);
    viewport.setBounds({0.f, 0.f, 200.f, 100.f});
    content.setBounds({0.f, 0.f, 150.f, 60.f});
    viewport.setContent(content);

    check(samePoint(viewport.getMaximumViewPosition(), {0.f, 0.f}));
    check(viewport.getHorizontalTrackBounds().isEmpty());
    check(viewport.getVerticalTrackBounds().isEmpty(), "no bars");
    check(sameRect(content.getBounds(), {0.f, 0.f, 150.f, 60.f}),
          "the content keeps its own size");

    host.mouseWheel(wheel({-10.f, -10.f}, true));
    check(parent.wheels == 1, "the wheel went on to the parent");

    viewport.setViewPosition({20.f, 20.f});
    check(samePoint(viewport.getViewPosition(), {0.f, 0.f}));
};

auto tResize = test("Viewport/aResizeClampsTheViewPosition") = []
{
    auto harness = Harness {};
    auto& viewport = harness.viewport;

    viewport.setViewPosition({408.f, 308.f});
    harness.changes = 0;

    viewport.setBounds({0.f, 0.f, 400.f, 300.f});

    check(samePoint(viewport.getMaximumViewPosition(), {208.f, 108.f}));
    check(samePoint(viewport.getViewPosition(), {208.f, 108.f}));
    check(harness.changes == 1 && samePoint(harness.lastReported, {208.f, 108.f}));
    check(sameRect(harness.content.getBounds(), {-208.f, -108.f, 600.f, 400.f}));

    viewport.setBounds({0.f, 0.f, 700.f, 500.f});

    check(samePoint(viewport.getViewPosition(), {0.f, 0.f}), "grown past it");
    check(viewport.getHorizontalTrackBounds().isEmpty());
};

auto tThumbGeometry = test("Viewport/theThumbsMeasureTheViewAndItsPosition") = []
{
    auto harness = Harness {};
    auto& viewport = harness.viewport;

    check(sameRect(viewport.getHorizontalTrackBounds(), {0.f, 92.f, 192.f, 8.f}));
    check(sameRect(viewport.getVerticalTrackBounds(), {192.f, 0.f, 8.f, 92.f}));

    auto across = viewport.getHorizontalThumbBounds();
    check(across.x == 0.f && near(across.w, 192.f * 192.f / 600.f),
          "the part of the content in view");

    auto upAndDown = viewport.getVerticalThumbBounds();
    check(upAndDown.h == 24.f, "never shorter than the minimum");

    viewport.setViewPosition({204.f, 308.f});

    across = viewport.getHorizontalThumbBounds();
    check(near(across.x, (192.f - across.w) * 0.5f), "halfway");
    check(viewport.getVerticalThumbBounds().y == 92.f - 24.f, "at the end");

    viewport.setScrollbarsShown(false, true);

    check(viewport.getHorizontalTrackBounds().isEmpty());
    check(sameRect(viewport.getViewArea(), {0.f, 0.f, 192.f, 100.f}));
    check(sameRect(viewport.getVerticalTrackBounds(), {192.f, 0.f, 8.f, 100.f}));
    check(samePoint(viewport.getMaximumViewPosition(), {408.f, 300.f}),
          "a hidden bar still scrolls, it just takes no room");
};

auto tThumbDrag = test("Viewport/draggingAThumbScrollsAndATrackClickPages") = []
{
    auto harness = Harness {};
    auto& viewport = harness.viewport;
    auto& host = harness.host;

    auto thumb = viewport.getHorizontalThumbBounds();
    auto grab = thumb.center();
    auto travel = 192.f - thumb.w;

    host.mouseDown(mouseAt(grab, 1.0));
    host.mouseDragged(mouseAt({grab.x + travel * 0.5f, grab.y}, 1.1));

    check(near(viewport.getViewPosition().x, 204.f),
          "half the travel, half the way");
    check(viewport.getViewPosition().y == 0.f);

    host.mouseDragged(mouseAt({grab.x + travel * 2.f, grab.y + 40.f}, 1.2));
    check(viewport.getViewPosition().x == 408.f, "clamped at the end");

    host.mouseUp(mouseAt({grab.x + travel * 2.f, grab.y + 40.f}, 1.3));
    check(harness.content.presses == 0, "the content under the bar saw nothing");

    viewport.setViewPosition({0.f, 0.f});

    host.mouseDown(mouseAt({196.f, 80.f}, 2.0));
    host.mouseUp(mouseAt({196.f, 80.f}, 2.1));

    check(samePoint(viewport.getViewPosition(), {0.f, 92.f}),
          "a click below the thumb pages down by the view's height");

    host.mouseDown(mouseAt({196.f, 2.f}, 3.0));
    host.mouseUp(mouseAt({196.f, 2.f}, 3.1));

    check(samePoint(viewport.getViewPosition(), {0.f, 0.f}), "and above it, up");

    host.mouseDown(mouseAt({100.f, 50.f}, 4.0));
    host.mouseUp(mouseAt({100.f, 50.f}, 4.1));

    check(harness.content.presses == 1, "a press off the bars is the content's");
};

auto tFlingAcross = test("Viewport/aFingerFlingsAcross") = []
{
    auto harness = Harness {{0.f, 0.f, 2000.f, 80.f}};
    auto& viewport = harness.viewport;

    harness.swipe({180.f, 50.f}, {60.f, 52.f}, 6, 0.01);

    auto released = viewport.getViewPosition().x;

    check(released > 0.f, "the content followed the finger");
    check(viewport.isFlinging() && harness.host.isAnimating());

    harness.host.advanceAnimations(frame);
    auto first = viewport.getViewPosition().x - released;

    harness.host.advanceAnimations(frame);
    auto second = viewport.getViewPosition().x - released - first;

    check(first > 0.f && second < first, "carries on, slowing");

    auto frames = harness.runFling();

    check(frames < 2000 && !viewport.isFlinging() && !harness.host.isAnimating());
    check(viewport.getViewPosition().x > released + 300.f);
    check(viewport.getViewPosition().y == 0.f, "and never moved up or down");
};

auto tFlingBoth = test("Viewport/aDiagonalFlingStopsEachAxisAtItsOwnEdge") = []
{
    auto harness = Harness {};
    auto& viewport = harness.viewport;

    viewport.setViewPosition({380.f, 0.f});
    harness.swipe({150.f, 80.f}, {30.f, 20.f}, 6, 0.01);

    check(viewport.isFlinging());

    harness.runFling();

    check(viewport.getViewPosition().x == 408.f, "across stopped at its edge");
    check(viewport.getViewPosition().y > 60.f, "while down carried on");
    check(!harness.host.isAnimating());
};

auto tTouchAlongFittingAxis =
    test("Viewport/aFingerAcrossContentThatOnlyScrollsDownIsNotTaken") = []
{
    auto harness = Harness {{0.f, 0.f, 150.f, 400.f}};
    auto& viewport = harness.viewport;

    check(viewport.getMaximumViewPosition().x == 0.f);

    harness.swipe({150.f, 50.f}, {20.f, 52.f}, 5, 0.05);

    check(samePoint(viewport.getViewPosition(), {0.f, 0.f}));
    check(!viewport.isFlinging());
};

auto tReplaceContent =
    test("Viewport/newContentReplacesTheOldAndStartsAtTheTop") = []
{
    auto harness = Harness {};
    auto& viewport = harness.viewport;
    auto other = Component {};

    viewport.setViewPosition({100.f, 100.f});
    other.setBounds({0.f, 0.f, 300.f, 1000.f});
    harness.changes = 0;

    viewport.setContent(other);

    check(viewport.getContent() == &other);
    check(harness.content.getParentComponent() == nullptr, "the old one is let go");
    check(other.getParentComponent() == &viewport);
    check(samePoint(viewport.getViewPosition(), {0.f, 0.f}));
    check(harness.changes == 1);
    check(sameRect(other.getBounds(), {0.f, 0.f, 300.f, 1000.f}),
          "and keeps its size");
    check(samePoint(viewport.getMaximumViewPosition(), {108.f, 908.f}));

    viewport.setViewPosition({50.f, 500.f});
    viewport.setContentSize(300.f, 400.f);

    check(sameRect(other.getBounds(), {-50.f, -308.f, 300.f, 400.f}),
          "resized in place, the view clamped to what is left");
};

auto tBackground = test("Viewport/isTransparentUnlessAsked") = []
{
    auto viewport = Viewport {};

    check(!viewport.getDrawsBackground());

    viewport.setDrawsBackground(true);
    check(viewport.getDrawsBackground());
};
} // namespace ViewportTests
