#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// A finger scrolling a panel or a list, and the safe area the root is laid out
// in. Nothing is rendered and no display link runs: every event carries its own
// time and the host's animations are stepped by hand, so a fling is the same
// numbers on every run.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

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

eacp::Graphics::MouseEvent touchAt(Point position, double time)
{
    return pointerAt(position, time, true);
}

struct Content final : Component
{
    Content()
    {
        button.onClick = [this] { ++clicks; };
        addAndMakeVisible(button);
        addAndMakeVisible(slider);
    }

    void resized() override
    {
        button.setBounds({0.f, 100.f, getWidth(), 40.f});
        slider.setBounds({0.f, 200.f, getWidth(), 40.f});
    }

    Button button {"Press"};
    Slider slider;
    int clicks = 0;
};

struct PanelHarness
{
    PanelHarness()
    {
        host.setAnimationClockEnabled(false);
        host.setBounds({0.f, 0.f, 200.f, 300.f});
        host.setRootComponent(root);

        root.addAndMakeVisible(panel);
        panel.setBounds({0.f, 0.f, 200.f, 300.f});

        content.setBounds({0.f, 0.f, 200.f, 2000.f});
        panel.setContent(content);

        content.slider.onDragStart = [this] { ++dragStarts; };
        content.slider.onDragEnd = [this] { ++dragEnds; };
    }

    // A finger dragged from `from` to `to` in `steps` moves `seconds` apart.
    void swipe(Point from, Point to, int steps, double seconds, bool lift = true)
    {
        host.mouseDown(touchAt(from, time));

        for (auto step = 1; step <= steps; ++step)
        {
            auto t = (float) step / (float) steps;
            time += seconds;
            host.mouseDragged(touchAt(
                {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t}, time));
        }

        if (lift)
            host.mouseUp(touchAt(to, time));
    }

    void tap(Point position)
    {
        host.mouseDown(touchAt(position, time));
        host.mouseUp(touchAt(position, time + 0.05));
        time += 0.1;
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
    ScrollPanel panel;
    Content content;

    double time = 10.0;
    int dragStarts = 0;
    int dragEnds = 0;
};

struct RowModel final : ListBoxModel
{
    int getNumRows() override { return 100; }

    void paintRow(UI::Graphics&, int, const Rect&, bool) override {}

    void selectedRowChanged(int row) override
    {
        lastSelected = row;
        ++selections;
    }

    int lastSelected = -1;
    int selections = 0;
};

struct ListHarness
{
    ListHarness()
    {
        host.setAnimationClockEnabled(false);
        host.setBounds({0.f, 0.f, 200.f, 200.f});
        host.setRootComponent(root);

        root.addAndMakeVisible(list);
        list.setBounds({0.f, 0.f, 200.f, 200.f});
        list.setRowHeight(20.f);
        list.setModel(&model);
    }

    ComponentHost host;
    Component root;
    RowModel model;
    ListBox list;
};
} // namespace

auto tSlopIsATap = test("TouchScroll/movementInsideTheSlopIsStillATap") = []
{
    auto harness = PanelHarness {};

    harness.swipe({100.f, 120.f}, {103.f, 114.f}, 3, 0.02);

    check(harness.panel.getScrollPosition() == 0.f, "nothing scrolled");
    check(harness.content.clicks == 1, "and the button was clicked");
    check(!harness.host.isAnimating());
};

auto tDragScrolls =
    test("TouchScroll/aDragPastTheSlopScrollsAndCancelsTheChild") = []
{
    auto harness = PanelHarness {};

    harness.swipe({100.f, 120.f}, {100.f, 20.f}, 10, 0.5, false);

    check(harness.panel.getScrollPosition() == 100.f - touchSlop,
          "the content followed the finger from the edge of the slop");

    harness.host.mouseUp(touchAt({100.f, 20.f}, harness.time + 0.5));

    check(harness.content.clicks == 0, "the button the finger started on is not");
    check(!harness.host.isAnimating(), "a finger held still before lifting");
    check(!harness.panel.isFlinging());
};

auto tFling = test("TouchScroll/aFlingCoastsSlowsAndStops") = []
{
    auto harness = PanelHarness {};

    harness.swipe({100.f, 250.f}, {100.f, 130.f}, 6, 0.01);

    auto released = harness.panel.getScrollPosition();

    check(harness.panel.isFlinging(), "let go moving, the content carries on");
    check(harness.host.isAnimating());

    harness.host.advanceAnimations(frame);
    auto first = harness.panel.getScrollPosition() - released;

    harness.host.advanceAnimations(frame);
    auto second = harness.panel.getScrollPosition() - released - first;

    check(first > 0.f, "in the direction the finger was going");
    check(second < first, "and slower each frame");

    auto frames = harness.runFling();

    check(frames < 2000, "it stops of its own accord");
    check(!harness.panel.isFlinging() && !harness.host.isAnimating());

    // Two seconds' worth of a 2000 point-per-second fling, give or take.
    auto travelled = harness.panel.getScrollPosition() - released;
    check(travelled > 600.f && travelled < 1200.f);
    check(harness.content.clicks == 0);
};

auto tFlingRepeats = test("TouchScroll/theSameFlingScrollsTheSameDistance") = []
{
    auto distance = []
    {
        auto harness = PanelHarness {};
        harness.swipe({100.f, 250.f}, {100.f, 150.f}, 5, 0.01);
        harness.runFling();
        return harness.panel.getScrollPosition();
    };

    check(distance() == distance());
};

auto tFlingEdge = test("TouchScroll/aFlingStopsAtTheEdge") = []
{
    auto harness = PanelHarness {};

    harness.panel.setScrollPosition(1650.f);
    harness.swipe({100.f, 250.f}, {100.f, 100.f}, 5, 0.01);

    auto frames = harness.runFling();

    check(harness.panel.getScrollPosition() == 1700.f, "clamped at the bottom");
    check(frames < 30, "and stopped there rather than pushing against it");
    check(!harness.host.isAnimating());
};

auto tPressStopsFling =
    test("TouchScroll/aPressDuringAFlingStopsItAndClicksNothing") = []
{
    auto harness = PanelHarness {};

    harness.swipe({100.f, 250.f}, {100.f, 130.f}, 6, 0.01);
    harness.host.advanceAnimations(frame);

    check(harness.panel.isFlinging());

    // The button brought under the finger, so the press lands on it: a press
    // on a moving list only stops it, whatever it lands on.
    auto stoppedAt = harness.panel.getScrollPosition();
    harness.content.button.setBounds({0.f, stoppedAt + 100.f, 200.f, 40.f});

    harness.tap({100.f, 120.f});

    check(!harness.panel.isFlinging() && !harness.host.isAnimating());
    check(harness.content.clicks == 0);

    harness.host.advanceAnimations(frame);
    check(harness.panel.getScrollPosition() == stoppedAt, "and it stays stopped");
};

auto tMouseDrag = test("TouchScroll/aMouseDragDoesNotScroll") = []
{
    auto harness = PanelHarness {};

    harness.host.mouseDown(pointerAt({100.f, 120.f}, 0.0, false));
    harness.host.mouseDragged(pointerAt({100.f, 20.f}, 0.1, false));
    harness.host.mouseUp(pointerAt({100.f, 20.f}, 0.2, false));

    check(harness.panel.getScrollPosition() == 0.f);
    check(!harness.host.isAnimating());
    check(harness.content.clicks == 0, "a press dragged off a button is a cancel");
};

auto tWheel = test("TouchScroll/theWheelStillScrolls") = []
{
    auto harness = PanelHarness {};

    auto wheel = pointerAt({100.f, 120.f}, 0.0, false);
    wheel.delta = {0.f, -2.f};
    harness.host.mouseWheel(wheel);

    check(harness.panel.getScrollPosition() == 80.f, "two lines of forty points");

    harness.swipe({100.f, 250.f}, {100.f, 130.f}, 6, 0.01);
    check(harness.panel.isFlinging());

    harness.host.mouseWheel(wheel);
    check(!harness.panel.isFlinging() && !harness.host.isAnimating(),
          "and the wheel stops a fling");
};

auto tSliderKeepsDrag =
    test("TouchScroll/aSliderDraggedAlongItsTrackKeepsTheDrag") = []
{
    auto harness = PanelHarness {};
    auto& slider = harness.content.slider;

    harness.swipe({20.f, 220.f}, {180.f, 222.f}, 8, 0.02);

    check(harness.panel.getScrollPosition() == 0.f, "across the panel's axis");
    check(slider.getValue() > 0.85f, "the thumb followed the finger");
    check(harness.dragStarts == 1 && harness.dragEnds == 1);
};

auto tSliderCancelled =
    test("TouchScroll/aScrollStartingOnASliderPutsItsValueBack") = []
{
    auto harness = PanelHarness {};
    auto& slider = harness.content.slider;

    slider.setValue(0.25f);

    harness.swipe({150.f, 220.f}, {152.f, 120.f}, 8, 0.05);

    check(harness.panel.getScrollPosition() > 0.f, "the finger scrolled");
    check(slider.getValue() == 0.25f, "the value the press moved is restored");
    check(harness.dragStarts == 1 && harness.dragEnds == 1,
          "and the gesture is closed exactly once");
};

auto tListTouch = test("TouchScroll/aListFollowsTheFingerAndSelectsOnATap") = []
{
    auto harness = ListHarness {};
    auto& host = harness.host;

    host.mouseDown(touchAt({50.f, 150.f}, 1.0));

    check(harness.model.selections == 0, "a finger does not select on the press");

    host.mouseDragged(touchAt({50.f, 100.f}, 1.5));
    host.mouseUp(touchAt({50.f, 100.f}, 2.0));

    check(harness.list.getScrollPosition() == 50.f - touchSlop);
    check(harness.model.selections == 0, "nor on a scroll");

    host.mouseDown(touchAt({50.f, 10.f}, 3.0));
    host.mouseUp(touchAt({50.f, 10.f}, 3.05));

    check(harness.model.selections == 1);
    check(harness.model.lastSelected == 2, "the row under the finger, scrolled");

    host.mouseDown(touchAt({50.f, 190.f}, 4.0));
    for (auto step = 1; step <= 5; ++step)
        host.mouseDragged(
            touchAt({50.f, 190.f - 24.f * (float) step}, 4.0 + 0.01 * step));
    host.mouseUp(touchAt({50.f, 70.f}, 4.05));

    check(harness.list.isFlinging() && host.isAnimating());

    host.advanceAnimations(frame);

    host.mouseDown(touchAt({50.f, 10.f}, 5.0));
    host.mouseUp(touchAt({50.f, 10.f}, 5.05));

    check(!harness.list.isFlinging(), "a press stops the list");
    check(harness.model.selections == 1, "and selects nothing");

    host.mouseDown(pointerAt({50.f, 10.f}, 6.0, false));
    check(harness.model.selections == 2, "a mouse still selects on the press");
    host.mouseUp(pointerAt({50.f, 10.f}, 6.1, false));
};

auto tListInPanel = test("TouchScroll/aListInsideAPanelScrollsItselfFirst") = []
{
    auto harness = PanelHarness {};
    auto model = RowModel {};
    auto list = ListBox {};

    list.setModel(&model);
    list.setRowHeight(20.f);
    harness.content.addAndMakeVisible(list);
    list.setBounds({0.f, 0.f, 200.f, 100.f});

    harness.swipe({100.f, 90.f}, {100.f, 20.f}, 5, 0.5);

    check(list.getScrollPosition() == 70.f - touchSlop, "the list took the finger");
    check(harness.panel.getScrollPosition() == 0.f, "and the panel did not move");
};

auto tSafeAreaInsets = test("SafeArea/theRootIsLaidOutInsideTheInsets") = []
{
    auto host = ComponentHost {};
    auto root = Component {};
    auto button = Button {};
    auto clicks = 0;

    button.onClick = [&clicks] { ++clicks; };
    root.addAndMakeVisible(button);
    button.setBounds({0.f, 0.f, 100.f, 30.f});

    host.setBounds({0.f, 0.f, 400.f, 800.f});
    host.setRootComponent(root);

    check(sameRect(root.getBounds(), {0.f, 0.f, 400.f, 800.f}),
          "a desktop window has no insets");

    host.setSafeAreaInsets({.top = 40.f, .left = 10.f, .bottom = 300.f});

    check(sameRect(root.getBounds(), {10.f, 40.f, 390.f, 460.f}),
          "the status bar, the cutout and the keyboard are left clear");
    check(sameRect(host.getRootBounds(), root.getBounds()));

    // A press is measured from the root's corner, not the view's.
    host.mouseDown(pointerAt({15.f, 45.f}, 0.0, false));
    host.mouseUp(pointerAt({15.f, 45.f}, 0.1, false));

    check(clicks == 1, "a press at the root's corner lands on what is there");

    host.mouseDown(pointerAt({15.f, 15.f}, 0.2, false));
    host.mouseUp(pointerAt({15.f, 15.f}, 0.3, false));

    check(clicks == 1, "and one under the status bar does not");

    // With no window the platform's layout pass never comes, so the resize it
    // would deliver is delivered here.
    host.setBounds({0.f, 0.f, 500.f, 900.f});
    host.resized();

    check(sameRect(root.getBounds(), {10.f, 40.f, 490.f, 560.f}),
          "a resize keeps the insets");
};

auto tSafeAreaOptOut = test("SafeArea/optingOutLaysTheRootOverTheWholeView") = []
{
    auto host = ComponentHost {};
    auto root = Component {};

    host.setBounds({0.f, 0.f, 400.f, 800.f});
    host.setRootComponent(root);
    host.setSafeAreaInsets({.top = 40.f, .bottom = 30.f});

    check(host.getRespectsSafeArea(), "on by default");
    check(sameRect(root.getBounds(), {0.f, 40.f, 400.f, 730.f}));

    host.setRespectsSafeArea(false);

    check(sameRect(root.getBounds(), {0.f, 0.f, 400.f, 800.f}));

    host.setSafeAreaInsets({.top = 60.f});

    check(sameRect(root.getBounds(), {0.f, 0.f, 400.f, 800.f}),
          "and stays there as the insets change");

    host.setRespectsSafeArea(true);

    check(sameRect(root.getBounds(), {0.f, 60.f, 400.f, 740.f}));
};

auto tSafeAreaKeyboard =
    test("SafeArea/aKeyboardCoveringAFocusedEditorScrollsItIntoView") = []
{
    struct Root final : Component
    {
        void resized() override { panel.setBounds(getLocalBounds()); }

        ScrollPanel panel;
    };

    auto host = ComponentHost {};
    auto root = Root {};
    auto content = Component {};
    auto editor = TextEditor {};

    root.addAndMakeVisible(root.panel);
    content.addAndMakeVisible(editor);
    content.setBounds({0.f, 0.f, 400.f, 800.f});
    editor.setBounds({0.f, 700.f, 400.f, 30.f});
    root.panel.setContent(content);

    host.setBounds({0.f, 0.f, 400.f, 800.f});
    host.setRootComponent(root);
    editor.grabKeyboardFocus();

    check(root.panel.getScrollPosition() == 0.f);

    host.setSafeAreaInsets({.bottom = 300.f});

    check(root.panel.getScrollPosition() == 230.f,
          "the editor's bottom edge sits on the keyboard's top");
};

auto tEditorFocusOnTap =
    test("TouchScroll/anEditorIsFocusedByATapAndNotByAScrollStartingOnIt") = []
{
    auto harness = PanelHarness {};
    auto editor = TextEditor {"text"};

    harness.content.addAndMakeVisible(editor);
    editor.setBounds({0.f, 250.f, 200.f, 40.f});

    harness.swipe({100.f, 270.f}, {100.f, 70.f}, 10, 0.5);

    check(harness.panel.getScrollPosition() > 0.f, "the panel scrolled");
    check(!editor.hasKeyboardFocus(), "and the editor was not focused");
    check(!harness.host.getProperties().wantsTextInput, "so no keyboard");

    harness.runFling();
    harness.panel.setScrollPosition(0.f);
    harness.host.mouseDown(touchAt({100.f, 270.f}, harness.time));

    check(!editor.hasKeyboardFocus(), "a finger does not focus on the press");

    harness.host.mouseUp(touchAt({102.f, 268.f}, harness.time + 0.05));

    check(editor.hasKeyboardFocus(), "but a tap focuses on the release");
    check(harness.host.getProperties().wantsTextInput);

    editor.giveAwayKeyboardFocus();
    harness.host.mouseDown(pointerAt({100.f, 270.f}, harness.time + 1.0, false));

    check(editor.hasKeyboardFocus(), "a mouse still focuses on the press");

    harness.host.mouseUp(pointerAt({100.f, 270.f}, harness.time + 1.1, false));
};

auto tTapClickFocuses =
    test("TouchScroll/aTapWhoseClickFocusesAnEditorLeavesItFocused") = []
{
    auto harness = PanelHarness {};
    auto checkbox = Checkbox {"Edit"};
    auto editor = TextEditor {"text"};

    harness.content.addAndMakeVisible(checkbox);
    harness.content.addAndMakeVisible(editor);
    checkbox.setBounds({0.f, 20.f, 200.f, 30.f});
    editor.setBounds({0.f, 60.f, 200.f, 30.f});

    auto focusEditor = [&editor](bool) { editor.grabKeyboardFocus(); };
    checkbox.onChange = focusEditor;

    harness.tap({20.f, 35.f});

    check(checkbox.isChecked(), "the tap clicked the box");
    check(editor.hasKeyboardFocus(), "and the focus its click gave is kept");
    check(harness.host.getProperties().wantsTextInput);
};

auto tTapStoppingFlingKeepsFocus =
    test("TouchScroll/aTapThatOnlyStopsAFlingMovesNoFocus") = []
{
    auto harness = PanelHarness {};
    auto editor = TextEditor {"text"};

    harness.root.setWantsKeyboardFocus(true);
    harness.content.addAndMakeVisible(editor);
    editor.setBounds({0.f, 1900.f, 200.f, 30.f});
    editor.grabKeyboardFocus();

    harness.swipe({100.f, 250.f}, {100.f, 130.f}, 6, 0.01);
    harness.host.advanceAnimations(frame);

    check(harness.panel.isFlinging());

    harness.tap({100.f, 120.f});

    check(!harness.panel.isFlinging());
    check(editor.hasKeyboardFocus(),
          "the panel's focusable parent did not take it from the editor");
};

auto tDestroyedParentStopsChildFling =
    test("TouchScroll/aFlingingChildOfADestroyedParentIsForgotten") = []
{
    auto host = ComponentHost {};
    auto root = Component {};
    auto parent = makeOwned<Component>();
    auto panel = makeOwned<ScrollPanel>();
    auto content = Component {};

    host.setAnimationClockEnabled(false);
    host.setBounds({0.f, 0.f, 200.f, 300.f});
    host.setRootComponent(root);

    root.addAndMakeVisible(*parent);
    parent->setBounds({0.f, 0.f, 200.f, 300.f});
    parent->addAndMakeVisible(*panel);
    panel->setBounds({0.f, 0.f, 200.f, 300.f});
    content.setBounds({0.f, 0.f, 200.f, 2000.f});
    panel->setContent(content);

    host.mouseDown(touchAt({100.f, 250.f}, 1.0));

    for (auto step = 1; step <= 6; ++step)
        host.mouseDragged(
            touchAt({100.f, 250.f - 20.f * (float) step}, 1.0 + 0.01 * step));

    host.mouseUp(touchAt({100.f, 130.f}, 1.06));

    check(panel->isFlinging() && host.isAnimating());

    parent.reset();

    check(!host.isAnimating(), "the parent took its child's fling with it");

    panel.reset();
    host.advanceAnimations(frame);

    check(!host.isAnimating());
};

auto tDestroyedParentDropsChildFocus =
    test("TouchScroll/aDestroyedParentTakesItsChildsPressAndFocusWithIt") = []
{
    auto host = ComponentHost {};
    auto root = Component {};
    auto parent = makeOwned<Component>();
    auto editor = makeOwned<TextEditor>("text");

    host.setBounds({0.f, 0.f, 200.f, 300.f});
    host.setRootComponent(root);

    root.addAndMakeVisible(*parent);
    parent->setBounds({0.f, 0.f, 200.f, 300.f});
    parent->addAndMakeVisible(*editor);
    editor->setBounds({0.f, 0.f, 200.f, 30.f});

    host.mouseDown(pointerAt({20.f, 15.f}, 1.0, false));

    check(editor->hasKeyboardFocus());

    parent.reset();
    editor.reset();

    check(host.getFocusedComponent() == nullptr);

    host.mouseDragged(pointerAt({30.f, 15.f}, 1.1, false));
    host.mouseUp(pointerAt({30.f, 15.f}, 1.2, false));
    host.keyDown(KeyEvent {});

    check(host.getFocusedComponent() == nullptr, "and nothing reached it after");
};
