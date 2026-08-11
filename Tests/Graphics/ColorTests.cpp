#include "Common.h"

// The static_asserts are the real test: a definition that stops being constexpr
// fails the build here rather than at a call site deep in an app.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
constexpr auto defaultColor = Color {};
constexpr auto rgb = Color {0.25f, 0.5f, 0.75f};
constexpr auto rgba = Color {0.25f, 0.5f, 0.75f, 0.5f};
constexpr auto grayed = Color::gray(0.5f);
constexpr auto white = Color::white();
constexpr auto black = Color::black(0.25f);
constexpr auto faded = rgb.withAlpha(0.5f);
constexpr auto lighter = rgb.brighter(0.1f);
constexpr auto darker = rgb.darker(0.1f);

static_assert(defaultColor.a == 1.f, "a default Color is opaque");
static_assert(rgb.r == 0.25f && rgb.g == 0.5f && rgb.b == 0.75f);
static_assert(rgb.a == 1.f, "the alpha argument defaults to opaque");
static_assert(rgba.a == 0.5f);
static_assert(grayed.r == grayed.g && grayed.g == grayed.b);
static_assert(white.r == 1.f && white.a == 1.f);
static_assert(black.r == 0.f && black.a == 0.25f);
static_assert(faded.a == 0.5f && faded.r == rgb.r, "withAlpha keeps the hue");
static_assert(lighter.b > rgb.b);
static_assert(darker.b < rgb.b);

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

static_assert(Color::white().brighter(0.5f).r == 1.f, "brighter clamps at 1");
static_assert(Color::black().darker(0.5f).r == 0.f, "darker clamps at 0");
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
