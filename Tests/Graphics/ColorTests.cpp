#include "Common.h"

// Color's constructors and withAlpha are constexpr so a theme -- a table of
// named colours -- can be a compile-time constant rather than something built
// during static initialisation.
//
// The static_asserts are the real test: they fail the *build* if a definition
// moves back out of line, which is the only way this property can regress. The
// runtime checks that follow exist so the arithmetic itself stays covered.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
// Every form a palette would use, evaluated at compile time. A constructor
// that stops being constexpr breaks compilation here, not at some call site
// deep in an app.
constexpr auto defaultColor = Color {};
constexpr auto rgb = Color {0.25f, 0.5f, 0.75f};
constexpr auto rgba = Color {0.25f, 0.5f, 0.75f, 0.5f};
constexpr auto grayed = Color::gray(0.5f);
constexpr auto white = Color::white();
constexpr auto black = Color::black(0.25f);
constexpr auto faded = rgb.withAlpha(0.5f);

static_assert(defaultColor.a == 1.f, "a default Color is opaque");
static_assert(rgb.r == 0.25f && rgb.g == 0.5f && rgb.b == 0.75f);
static_assert(rgb.a == 1.f, "the alpha argument defaults to opaque");
static_assert(rgba.a == 0.5f);
static_assert(grayed.r == grayed.g && grayed.g == grayed.b);
static_assert(white.r == 1.f && white.a == 1.f);
static_assert(black.r == 0.f && black.a == 0.25f);
static_assert(faded.a == 0.5f && faded.r == rgb.r, "withAlpha keeps the hue");

// A whole palette as one compile-time table, which is the shape this change
// exists to allow.
struct Theme
{
    Color background;
    Color foreground;
    Color accent;
};

constexpr auto theme = Theme {.background = Color {0.11f, 0.12f, 0.15f},
                              .foreground = Color::gray(0.9f),
                              .accent = Color {0.4f, 0.6f, 0.9f}};

static_assert(theme.accent.b == 0.9f);

// The named, packed and 8-bit forms are constexpr too, so a palette can be
// written the way a designer hands it over.
constexpr auto packed = Color::fromARGB(0x80ff0000u);
constexpr auto opaqueHex = Color::fromRGB(0x00ff00u);
constexpr auto bytes = Color::fromRGBA8(0, 0, 255, 255);
constexpr auto halfway = Color::black().interpolatedWith(Color::white(), 0.5f);

static_assert(packed.r == 1.f && packed.g == 0.f && packed.a == 128.f / 255.f);
static_assert(opaqueHex.g == 1.f && opaqueHex.a == 1.f);
static_assert(bytes.b == 1.f && bytes.r == 0.f);
static_assert(halfway.r == 0.5f && halfway.a == 1.f);
static_assert(Color::transparent().a == 0.f);
static_assert(Color::fromARGB(0xff336699u).toARGB() == 0xff336699u);
} // namespace

auto tColorDefaultsToOpaqueBlack = test("Color/defaultsToOpaqueBlack") = []
{
    auto color = Color {};

    check(color.r == 0.f);
    check(color.g == 0.f);
    check(color.b == 0.f);
    check(color.a == 1.f);
};

auto tColorWithAlphaKeepsChannels = test("Color/withAlphaKeepsChannels") = []
{
    auto color = Color {0.2f, 0.4f, 0.6f}.withAlpha(0.5f);

    check(color.r == 0.2f);
    check(color.g == 0.4f);
    check(color.b == 0.6f);
    check(color.a == 0.5f);
};

// brighter/darker move every channel by the same amount and leave alpha alone,
// which is what makes them usable for hover and pressed states.
auto tColorBrighterAndDarker = test("Color/brighterAndDarkerShiftChannels") = []
{
    auto base = Color {0.4f, 0.5f, 0.6f, 0.75f};

    auto up = base.brighter(0.2f);
    check(std::abs(up.r - 0.6f) < 0.0001f);
    check(std::abs(up.g - 0.7f) < 0.0001f);
    check(up.a == 0.75f);

    auto down = base.darker(0.2f);
    check(std::abs(down.r - 0.2f) < 0.0001f);
    check(std::abs(down.g - 0.3f) < 0.0001f);
    check(down.a == 0.75f);
};

auto tColorClampsAtBounds = test("Color/brighterAndDarkerClamp") = []
{
    auto light = Color {0.9f, 0.9f, 0.9f}.brighter(0.5f);
    check(light.r == 1.f);
    check(light.g == 1.f);
    check(light.b == 1.f);

    auto dark = Color {0.1f, 0.1f, 0.1f}.darker(0.5f);
    check(dark.r == 0.f);
    check(dark.g == 0.f);
    check(dark.b == 0.f);

    check(Color::white().brighter(0.5f).r == 1.f);
    check(Color::black().darker(0.5f).r == 0.f);
};

auto tColorBrighterAndDarkerMoveBlue =
    test("Color/brighterAndDarkerMoveEveryChannel") = []
{
    auto rgb = Color {0.25f, 0.5f, 0.75f};

    check(rgb.brighter(0.1f).b > rgb.b);
    check(rgb.darker(0.1f).b < rgb.b);
};

auto tColorHelpers = test("Color/grayWhiteBlackHelpers") = []
{
    auto gray = Color::gray(0.5f, 0.25f);
    check(gray.r == 0.5f && gray.g == 0.5f && gray.b == 0.5f);
    check(gray.a == 0.25f);

    check(Color::white().r == 1.f);
    check(Color::black().r == 0.f);
    check(Color::white(0.5f).a == 0.5f);
};

auto tColorNamedColors = test("Color/namedColorsArePurePrimaries") = []
{
    auto isRGBA = [](Color c, float r, float g, float b, float a)
    { return c.r == r && c.g == g && c.b == b && c.a == a; };

    check(isRGBA(Color::transparent(), 0.f, 0.f, 0.f, 0.f));
    check(isRGBA(Color::red(), 1.f, 0.f, 0.f, 1.f));
    check(isRGBA(Color::green(), 0.f, 1.f, 0.f, 1.f));
    check(isRGBA(Color::blue(), 0.f, 0.f, 1.f, 1.f));
    check(isRGBA(Color::yellow(), 1.f, 1.f, 0.f, 1.f));
    check(isRGBA(Color::cyan(), 0.f, 1.f, 1.f, 1.f));
    check(isRGBA(Color::magenta(), 1.f, 0.f, 1.f, 1.f));
    check(Color::red(0.25f).a == 0.25f);
};

// 0xAARRGGBB, the juce::Colour layout: the alpha byte comes first, so a value
// written without one is transparent.
auto tColorFromARGB = test("Color/fromARGBReadsAlphaFromTheTopByte") = []
{
    auto color = Color::fromARGB(0xff336699u);

    check(std::abs(color.r - 0x33 / 255.f) < 0.0001f);
    check(std::abs(color.g - 0x66 / 255.f) < 0.0001f);
    check(std::abs(color.b - 0x99 / 255.f) < 0.0001f);
    check(color.a == 1.f);

    check(Color::fromARGB(0x00ff0000u).a == 0.f);
};

auto tColorFromRGB = test("Color/fromRGBIsOpaqueAndIgnoresTheTopByte") = []
{
    auto color = Color::fromRGB(0x12336699u);

    check(color.a == 1.f);
    check(color.toARGB() == 0xff336699u);
    check(Color::fromRGB(0x336699u, 0.5f).a == 0.5f);
};

auto tColorFromRGBA8 = test("Color/fromRGBA8ScalesBytesToUnit") = []
{
    auto color = Color::fromRGBA8(255, 0, 51);

    check(color.r == 1.f);
    check(color.g == 0.f);
    check(std::abs(color.b - 0.2f) < 0.0001f);
    check(color.a == 1.f);
    check(Color::fromRGBA8(0, 0, 0, 0).a == 0.f);
};

auto tColorToARGB = test("Color/toARGBRoundsAndClamps") = []
{
    check(Color::white().toARGB() == 0xffffffffu);
    check(Color::transparent().toARGB() == 0x00000000u);
    check(Color {2.f, -1.f, 0.5f, 1.f}.toARGB() == 0xffff0080u);

    for (auto value = 0u; value <= 0xffu; ++value)
    {
        auto argb = (value << 24) | (value << 16) | (value << 8) | value;
        check(Color::fromARGB(argb).toARGB() == argb);
    }
};

auto tColorInterpolates = test("Color/interpolatedWithBlendsEveryChannel") = []
{
    auto from = Color {0.f, 0.2f, 1.f, 0.f};
    auto to = Color {1.f, 0.6f, 0.f, 1.f};

    auto quarter = from.interpolatedWith(to, 0.25f);
    check(std::abs(quarter.r - 0.25f) < 0.0001f);
    check(std::abs(quarter.g - 0.3f) < 0.0001f);
    check(std::abs(quarter.b - 0.75f) < 0.0001f);
    check(std::abs(quarter.a - 0.25f) < 0.0001f);

    auto start = from.interpolatedWith(to, 0.f);
    check(start.r == from.r && start.a == from.a);

    auto end = from.interpolatedWith(to, 1.f);
    check(end.r == to.r && end.b == to.b && end.a == to.a);
};

auto tColorInterpolationClamps = test("Color/interpolatedWithClampsProportion") = []
{
    auto from = Color::black();
    auto to = Color::white();

    check(from.interpolatedWith(to, -1.f).r == 0.f);
    check(from.interpolatedWith(to, 2.f).r == 1.f);
};
