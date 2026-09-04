#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// What a value control promises whatever is driving it, which is the half that
// has nothing to do with drawing.
//
// Two things are being pinned here. A value set from outside must not come back
// out as a change: a control attached to a parameter is told the parameter
// moved, and a report from that would be written straight back to the parameter
// it came from. And a gesture must be bracketed exactly once -- a host recording
// automation opens a recording on the first callback and closes it on the
// second, so an unmatched one leaves a parameter latched to the mouse until
// something else touches it.
//
// Both controls answer the same, so the checks are written once and run twice.
// The host is constructed but never rendered.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
eacp::Graphics::MouseEvent mouseAt(Point position, int clickCount = 1)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;
    event.clickCount = clickCount;

    return event;
}

template <typename Control>
struct Harness
{
    Harness()
    {
        host.setBounds({0.f, 0.f, 200.f, 100.f});
        host.setRootComponent(root);

        root.setBounds({0.f, 0.f, 200.f, 100.f});
        root.addAndMakeVisible(control);
        control.setBounds({0.f, 0.f, 200.f, 100.f});

        control.onValueChange = [this](float value)
        {
            lastValue = value;
            ++changes;
        };

        control.onDragStart = [this] { ++starts; };
        control.onDragEnd = [this] { ++ends; };
    }

    void press(Point position, int clickCount = 1)
    {
        host.mouseDown(mouseAt(position, clickCount));
    }

    void drag(Point position) { host.mouseDragged(mouseAt(position)); }
    void release(Point position) { host.mouseUp(mouseAt(position)); }

    ComponentHost host;
    Component root;
    Control control;

    float lastValue = 0.f;
    int changes = 0;
    int starts = 0;
    int ends = 0;
};

template <typename Control>
void checkSettingTheValueIsSilent()
{
    auto harness = Harness<Control> {};

    harness.control.setValue(0.8f);

    check(harness.control.getValue() == 0.8f);
    check(harness.changes == 0, "a value pushed in does not come back out");

    harness.control.setValue(0.2f, true);

    check(harness.changes == 1);
    check(harness.lastValue == 0.2f);

    harness.control.setValue(0.2f, true);

    check(harness.changes == 1, "and a value that did not move is not a change");
}

template <typename Control>
void checkTheMousePathReports()
{
    auto harness = Harness<Control> {};

    harness.press({150.f, 50.f});
    harness.drag({20.f, 10.f});

    check(harness.changes > 0, "what the pointer does is a change like any other");
    check(harness.lastValue == harness.control.getValue());
}

template <typename Control>
void checkTheGestureIsBracketed()
{
    auto harness = Harness<Control> {};

    harness.press({150.f, 50.f});

    check(harness.starts == 1);
    check(harness.ends == 0, "the gesture is still open");

    harness.drag({20.f, 10.f});

    check(harness.starts == 1, "and a drag does not open a second one");

    harness.release({20.f, 10.f});

    check(harness.starts == 1 && harness.ends == 1);
}

template <typename Control>
void checkDoubleClickResetsAsAWholeGesture()
{
    auto harness = Harness<Control> {};

    harness.control.setValue(0.9f);
    harness.control.setDefaultValue(0.25f);

    harness.press({150.f, 50.f}, 2);

    check(harness.control.getValue() == 0.25f);
    check(harness.changes == 1 && harness.lastValue == 0.25f);
    check(harness.starts == 1 && harness.ends == 1, "bracketed, and closed");

    // The host captures a press, so the release arrives here whatever the press
    // decided to do with it.
    harness.release({150.f, 50.f});

    check(harness.ends == 1, "and does not close the gesture a second time");
    check(harness.control.getValue() == 0.25f, "nor move off the default");
}

template <typename Control>
void checkDoubleClickWithNoDefaultIsAnOrdinaryPress()
{
    auto harness = Harness<Control> {};

    harness.press({150.f, 50.f}, 2);

    check(harness.starts == 1 && harness.ends == 0);

    harness.drag({20.f, 10.f});

    check(harness.changes > 0, "a control with nowhere to reset to still drags");

    harness.release({20.f, 10.f});

    check(harness.ends == 1);
}
} // namespace

auto tSliderSetValueIsSilent =
    test("Slider/settingTheValueReportsOnlyWhenAsked") = []
{ checkSettingTheValueIsSilent<Slider>(); };

auto tKnobSetValueIsSilent = test("Knob/settingTheValueReportsOnlyWhenAsked") = []
{ checkSettingTheValueIsSilent<Knob>(); };

auto tSliderMouseReports = test("Slider/theMousePathReportsWhatItSets") = []
{ checkTheMousePathReports<Slider>(); };

auto tKnobMouseReports = test("Knob/theMousePathReportsWhatItSets") = []
{ checkTheMousePathReports<Knob>(); };

auto tSliderBrackets = test("Slider/aDragIsBracketedExactlyOnce") = []
{ checkTheGestureIsBracketed<Slider>(); };

auto tKnobBrackets = test("Knob/aDragIsBracketedExactlyOnce") = []
{ checkTheGestureIsBracketed<Knob>(); };

auto tSliderDoubleClick = test("Slider/aDoubleClickResetsAsAWholeGesture") = []
{ checkDoubleClickResetsAsAWholeGesture<Slider>(); };

auto tKnobDoubleClick = test("Knob/aDoubleClickResetsAsAWholeGesture") = []
{ checkDoubleClickResetsAsAWholeGesture<Knob>(); };

auto tSliderDoubleClickNoDefault =
    test("Slider/aDoubleClickWithNoDefaultIsAnOrdinaryPress") = []
{ checkDoubleClickWithNoDefaultIsAnOrdinaryPress<Slider>(); };

auto tKnobDoubleClickNoDefault =
    test("Knob/aDoubleClickWithNoDefaultIsAnOrdinaryPress") = []
{ checkDoubleClickWithNoDefaultIsAnOrdinaryPress<Knob>(); };

// The fader's own arithmetic: where the pointer landed along the track is the
// value, which is the part a rotary control does differently.
auto tSliderTracksThePointer = test("Slider/thePositionOfThePressIsTheValue") = []
{
    auto harness = Harness<Slider> {};

    harness.press({150.f, 50.f});

    check(harness.control.getValue() == 0.75f);

    harness.drag({50.f, 50.f});

    check(harness.control.getValue() == 0.25f);
};

// And the knob's: vertical travel from where the drag started, so the hand does
// not have to trace the shape it is turning.
auto tKnobTracksVerticalTravel = test("Knob/theValueFollowsVerticalTravel") = []
{
    auto harness = Harness<Knob> {};

    harness.control.setValue(0.5f);
    harness.press({100.f, 50.f});

    check(harness.control.getValue() == 0.5f, "the press alone does not move it");

    // A hundred points up, against a two hundred point full sweep.
    harness.drag({100.f, -50.f});

    check(harness.control.getValue() == 1.f);

    harness.drag({100.f, 70.f});

    check(harness.control.getValue() == 0.4f, "and back down from the same origin");
};
