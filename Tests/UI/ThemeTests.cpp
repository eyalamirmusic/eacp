#include "Common.h"

#include <NanoTest/NanoTest.h>

// A theme is keys to values, one per host as the font is: what lets one product
// draw its stock widgets in its own colours, corners and face without every
// other tree in the process -- a plugin's editor beside the window it is hosted
// in -- changing with it.
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
const auto grey = Color {0.5f, 0.5f, 0.5f, 1.f};

// A key eacp has never heard of, as an app would declare one.
constexpr auto stripe = ColourKey {"themeTests.stripe"};
constexpr auto gutter = NumberKey {"themeTests.gutter"};

bool sameColour(const Color& a, const Color& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool sameFont(const Font& a, const Font& b)
{
    return a.family == b.family && a.pointSize == b.pointSize && a.style == b.style;
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

auto tHoldsWhatItIsGiven = test("Theme/aThemeHoldsWhatItIsGivenAndNothingElse") = []
{
    auto theme = Theme {};

    check(theme.isEmpty());
    check(theme.find(ThemeKeys::panel) == nullptr);
    check(theme.find(stripe) == nullptr);

    theme.set(ThemeKeys::panel, red);
    theme.set(stripe, blue);
    theme.set(gutter, 12.f);

    check(theme.size() == 3);
    check(theme.contains(ThemeKeys::panel) && theme.contains(stripe));
    check(sameColour(*theme.find(ThemeKeys::panel), red));
    check(sameColour(*theme.find(stripe), blue));
    check(*theme.find(gutter) == 12.f);

    // Setting again replaces rather than adds.
    theme.set(stripe, green);
    check(theme.size() == 3);
    check(sameColour(*theme.find(stripe), green));

    // The same name read as another type is not there.
    check(theme.find(NumberKey {"themeTests.stripe"}) == nullptr);
};

auto tFallback = test("Theme/readingFallsBackToTheDefaultThenToTheCaller") = []
{
    auto theme = Theme {};
    theme.set(ThemeKeys::panel, red);

    // Its own value first.
    check(sameColour(theme.get(ThemeKeys::panel), red));

    // Then eacp's, for a key of eacp's it does not hold.
    check(sameColour(theme.get(ThemeKeys::outline),
                     defaultTheme().get(ThemeKeys::outline)));
    check(theme.get(ThemeKeys::buttonCorner) == 5.f);

    // Then the caller's, for a key no one has set -- the default theme
    // included, which holds nothing of an app's.
    check(sameColour(theme.get(stripe, grey), grey));
    check(sameColour(defaultTheme().get(stripe, grey), grey));
    check(theme.get(gutter, 7.f) == 7.f);
    check(theme.get(gutter) == 0.f);

    // And a key the theme does hold wins over the caller's.
    theme.set(stripe, blue);
    check(sameColour(theme.get(stripe, grey), blue));

    // eacp's own fallback for the combo box's highlight: accentDim, until a
    // theme names one.
    check(sameColour(
        theme.get(ThemeKeys::comboBoxHighlight, theme.get(ThemeKeys::accentDim)),
        defaultTheme().get(ThemeKeys::accentDim)));
    theme.set(ThemeKeys::comboBoxHighlight, green);
    check(sameColour(
        theme.get(ThemeKeys::comboBoxHighlight, theme.get(ThemeKeys::accentDim)),
        green));
};

auto tDefaultLook = test("Theme/theDefaultIsTheLookEacpAlwaysDrew") = []
{
    const auto& theme = defaultTheme();

    check(sameColour(theme.get(ThemeKeys::background), {0.11f, 0.12f, 0.15f, 1.f}));
    check(sameColour(theme.get(ThemeKeys::panel), {0.16f, 0.17f, 0.21f, 1.f}));
    check(sameColour(theme.get(ThemeKeys::outline), {1.f, 1.f, 1.f, 0.10f}));
    check(sameColour(theme.get(ThemeKeys::text), {0.92f, 0.93f, 0.96f, 1.f}));
    check(sameColour(theme.get(ThemeKeys::dimText), {0.60f, 0.63f, 0.70f, 1.f}));
    check(sameColour(theme.get(ThemeKeys::accent), {0.35f, 0.62f, 0.95f, 1.f}));
    check(sameColour(theme.get(ThemeKeys::accentDim), {0.35f, 0.62f, 0.95f, 0.35f}));
    check(sameColour(theme.get(ThemeKeys::accentText), {0.08f, 0.09f, 0.12f, 1.f}));
    check(sameColour(theme.get(ThemeKeys::hover), {1.f, 1.f, 1.f, 0.07f}));
    check(sameColour(theme.get(ThemeKeys::pressed), {1.f, 1.f, 1.f, 0.14f}));
    check(sameFont(theme.get(ThemeKeys::font), {defaultUIFontFamily(), 13.f}));

    check(theme.get(ThemeKeys::buttonCorner) == 5.f);
    check(theme.get(ThemeKeys::checkboxCorner) == 4.f);
    check(theme.get(ThemeKeys::comboBoxCorner) == 5.f);
    check(theme.get(ThemeKeys::sliderCorner) == 4.f);
    check(theme.get(ThemeKeys::tabCorner) == 5.f);
    check(theme.get(ThemeKeys::textEditorCorner) == 4.f);

    // A combo box's highlight is accentDim's, by falling back, not by a value.
    check(!theme.contains(ThemeKeys::comboBoxHighlight));

    // A host draws in the same face and backdrop before any theme.
    auto host = ComponentHost {};
    check(sameFont(host.getFont(), theme.get(ThemeKeys::font)));
};

auto tHostStartsEmpty = test("Theme/aHostsThemeIsEmptyUntilSet") = []
{
    auto host = ComponentHost {};

    check(host.getTheme().isEmpty());
    check(sameColour(host.getTheme().get(ThemeKeys::panel),
                     defaultTheme().get(ThemeKeys::panel)));

    auto theme = Theme {};
    theme.set(ThemeKeys::panel, red);
    host.setTheme(theme);

    check(sameColour(host.getTheme().get(ThemeKeys::panel), red));
    check(sameColour(host.getTheme().get(ThemeKeys::accent),
                     defaultTheme().get(ThemeKeys::accent)));
};

auto tHostFace = test("Theme/aThemeThatHoldsAFaceGivesItToTheHost") = []
{
    auto host = ComponentHost {};
    host.setFontPointSize(17.f);

    // Colours alone leave the face as it was.
    auto colours = Theme {};
    colours.set(ThemeKeys::panel, red);
    host.setTheme(colours);
    check(host.getFont().pointSize == 17.f);

    auto face = Font {defaultUIFontFamily(), 11.5f, FontStyle::Bold};
    auto withFace = Theme {};
    withFace.set(ThemeKeys::font, face);
    host.setTheme(withFace);
    check(sameFont(host.getFont(), face));

    // And setFont after it is the last word, as setFont always is.
    host.setFontPointSize(20.f);
    check(host.getFont().pointSize == 20.f);
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

    check(sameColour(firstShape(recorder).colour, {0.16f, 0.17f, 0.21f, 1.f}));
    check(firstShape(recorder).cornerRadius == 5.f);
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

    auto theme = Theme {};
    theme.set(ThemeKeys::panel, red);
    theme.set(ThemeKeys::buttonCorner, 9.f);

    auto hosted = Hosted {root};
    hosted.host.setTheme(theme);

    // Asked of the host whatever depth the widget is at.
    check(&button.getHostTheme() == &hosted.host.getTheme());

    auto buttonPaint = Recorder {};
    button.paint(buttonPaint.g);

    const auto& shapes = buttonPaint.list.getShapes();
    check(sameColour(shapes[0].colour, red), "the theme's panel");
    check(shapes[0].cornerRadius == 9.f, "the theme's corner");

    // The outline, which the theme does not hold: eacp's.
    check(sameColour(shapes.back().colour, defaultTheme().get(ThemeKeys::outline)));

    auto boxPaint = Recorder {};
    box.paint(boxPaint.g);
    check(sameColour(firstShape(boxPaint).colour, red), "the theme's panel");
    check(firstShape(boxPaint).cornerRadius == 5.f, "eacp's corner for a box");
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

    auto theme = Theme {};
    theme.set(ThemeKeys::accent, blue);

    auto hosted = Hosted {root};
    hosted.host.setTheme(theme);

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
    auto caption = Label {"caption"};

    root.addAndMakeVisible(themed);
    root.addAndMakeVisible(own);
    root.addAndMakeVisible(caption);
    themed.setBounds({0.f, 0.f, 80.f, 24.f});
    own.setBounds({0.f, 30.f, 80.f, 24.f});
    caption.setBounds({0.f, 60.f, 80.f, 24.f});

    own.setColour(green);

    // Told which key before there is a theme to read it from.
    caption.setThemeColour(ThemeKeys::dimText);

    auto theme = Theme {};
    theme.set(ThemeKeys::text, blue);
    theme.set(ThemeKeys::dimText, grey);

    auto hosted = Hosted {root};
    hosted.host.setTheme(theme);

    auto glyphColour = [](Label& label)
    {
        auto recorder = Recorder {};
        recorder.text.setViewport({100.f, 50.f}, 1.f);
        label.paint(recorder.g);

        check(recorder.list.getGlyphRuns().size() == 1);
        return recorder.list.getGlyphRuns()[0].colour;
    };

    check(sameColour(glyphColour(themed), blue), "the theme's text");
    check(sameColour(glyphColour(own), green), "its own colour");
    check(sameColour(glyphColour(caption), grey), "the theme's dimText");

    // Whichever was said last.
    caption.setColour(red);
    check(sameColour(glyphColour(caption), red));
    caption.setThemeColour(ThemeKeys::text);
    check(sameColour(glyphColour(caption), blue));
};

auto tOtherHostsUntouched = test("Theme/anotherHostKeepsItsOwnTheme") = []
{
    if (!hasDevice())
        return;

    auto themedButton = Button {};
    auto plainButton = Button {};
    themedButton.setBounds({0.f, 0.f, 80.f, 24.f});
    plainButton.setBounds({0.f, 0.f, 80.f, 24.f});

    auto theme = Theme {};
    theme.set(ThemeKeys::panel, red);

    auto themed = Hosted {themedButton};
    auto plain = Hosted {plainButton};
    themed.host.setTheme(theme);

    auto plainPaint = Recorder {};
    plainButton.paint(plainPaint.g);

    check(sameColour(firstShape(plainPaint).colour,
                     defaultTheme().get(ThemeKeys::panel)));
};
} // namespace ThemeTests
