#include "Common.h"

#include <NanoTest/NanoTest.h>

// A theme per host, as the font is per host: what lets one product draw its
// stock widgets in its own colours without every other tree in the process --
// a plugin's editor beside the window it is hosted in -- changing with it.
//
// What is checked is the colour each paint() recorded, so nothing is rendered.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace ThemeTests
{
namespace
{
const auto green = Color {0.f, 1.f, 0.f, 1.f};

bool sameColour(const Color& a, const Color& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Every colour moved off the default, so a widget that still reads
// defaultTheme() shows up as the wrong one.
Theme customTheme()
{
    auto theme = Theme {};
    theme.background = {0.2f, 0.2f, 0.4f, 1.f};
    theme.panel = red;
    theme.outline = {0.5f, 0.5f, 0.5f, 1.f};
    theme.text = {1.f, 1.f, 1.f, 1.f};
    theme.dimText = {0.9f, 0.9f, 0.9f, 1.f};
    theme.accent = blue;
    theme.accentDim = {0.f, 0.f, 0.f, 1.f};

    return theme;
}

// A widget as the root of a host of its own.
struct Hosted
{
    explicit Hosted(Component& widget)
    {
        host.setBounds({0.f, 0.f, 200.f, 100.f});
        host.setRootComponent(widget);
    }

    ComponentHost host;
};

const ShapeDraw& firstShape(const Recorder& recorder)
{
    return recorder.list.getShapes()[0];
}
} // namespace

auto tHostStartsWithTheDefault = test("Theme/aHostStartsWithTheDefaultTheme") = []
{
    auto host = ComponentHost {};

    check(sameColour(host.getTheme().panel, defaultTheme().panel));
    check(sameColour(host.getTheme().accent, defaultTheme().accent));

    host.setTheme(customTheme());

    check(sameColour(host.getTheme().panel, red));
    check(sameColour(host.getTheme().accent, blue));
};

auto tOutsideAHost = test("Theme/aWidgetOutsideAHostDrawsTheDefault") = []
{
    if (!hasDevice())
        return;

    auto button = Button {};
    button.setBounds({0.f, 0.f, 80.f, 24.f});

    check(&button.getHostTheme() == &defaultTheme());

    auto recorder = Recorder {};
    button.paint(recorder.g);

    check(sameColour(firstShape(recorder).colour, defaultTheme().panel));
};

auto tInsideAHost = test("Theme/aWidgetDrawsInItsHostsTheme") = []
{
    if (!hasDevice())
        return;

    auto root = Component {};
    auto button = Button {"Press"};
    auto box = ComboBox {};

    root.addAndMakeVisible(button);
    root.addAndMakeVisible(box);
    button.setBounds({0.f, 0.f, 80.f, 24.f});
    box.setBounds({0.f, 30.f, 80.f, 24.f});

    auto hosted = Hosted {root};
    hosted.host.setTheme(customTheme());

    // Asked of the host whatever depth the widget is at.
    check(&button.getHostTheme() == &hosted.host.getTheme());

    auto buttonPaint = Recorder {};
    button.paint(buttonPaint.g);
    check(sameColour(firstShape(buttonPaint).colour, red), "the theme's panel");

    auto boxPaint = Recorder {};
    box.paint(boxPaint.g);
    check(sameColour(firstShape(boxPaint).colour, red), "the theme's panel");
};

auto tWidgetColourWins = test("Theme/aColourGivenToAWidgetWinsOverTheTheme") = []
{
    if (!hasDevice())
        return;

    auto root = Component {};
    auto themed = Checkbox {};
    auto own = Checkbox {};

    root.addAndMakeVisible(themed);
    root.addAndMakeVisible(own);
    themed.setBounds({0.f, 0.f, 80.f, 24.f});
    own.setBounds({0.f, 30.f, 80.f, 24.f});

    themed.setChecked(true);
    own.setChecked(true);
    own.setAccentColour(green);

    auto hosted = Hosted {root};
    hosted.host.setTheme(customTheme());

    auto themedPaint = Recorder {};
    themed.paint(themedPaint.g);
    check(sameColour(firstShape(themedPaint).colour, blue), "the theme's accent");

    auto ownPaint = Recorder {};
    own.paint(ownPaint.g);
    check(sameColour(firstShape(ownPaint).colour, green), "its own accent");
};

auto tLabelColour = test("Theme/aLabelDrawsInTheThemesTextUnlessToldOtherwise") = []
{
    if (!hasDevice())
        return;

    auto root = Component {};
    auto themed = Label {"themed"};
    auto own = Label {"own"};

    root.addAndMakeVisible(themed);
    root.addAndMakeVisible(own);
    themed.setBounds({0.f, 0.f, 80.f, 24.f});
    own.setBounds({0.f, 30.f, 80.f, 24.f});
    own.setColour(green);

    auto theme = customTheme();
    theme.text = blue;

    auto hosted = Hosted {root};
    hosted.host.setTheme(theme);

    auto themedPaint = Recorder {};
    themedPaint.text.setViewport({100.f, 50.f}, 1.f);
    themed.paint(themedPaint.g);

    auto ownPaint = Recorder {};
    ownPaint.text.setViewport({100.f, 50.f}, 1.f);
    own.paint(ownPaint.g);

    check(themedPaint.list.getGlyphRuns().size() == 1);
    check(sameColour(themedPaint.list.getGlyphRuns()[0].colour, blue));

    check(ownPaint.list.getGlyphRuns().size() == 1);
    check(sameColour(ownPaint.list.getGlyphRuns()[0].colour, green));
};

auto tOtherHostsUntouched = test("Theme/anotherHostKeepsItsOwnTheme") = []
{
    if (!hasDevice())
        return;

    auto themedButton = Button {};
    auto plainButton = Button {};
    themedButton.setBounds({0.f, 0.f, 80.f, 24.f});
    plainButton.setBounds({0.f, 0.f, 80.f, 24.f});

    auto themed = Hosted {themedButton};
    auto plain = Hosted {plainButton};
    themed.host.setTheme(customTheme());

    auto plainPaint = Recorder {};
    plainButton.paint(plainPaint.g);

    check(sameColour(firstShape(plainPaint).colour, defaultTheme().panel));
};
} // namespace ThemeTests
