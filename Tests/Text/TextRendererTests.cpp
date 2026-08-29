#include "Common.h"

#include <cmath>
#include <optional>

// TextRenderer: the layout loop, and above all where a glyph lands.
//
// A glyph is snapped to a whole device pixel and rasterized at the fraction
// the pen fell short of it, so that every texel lands on a pixel of its own
// and the linear sampler has nothing to blend. The snap is arithmetic and
// tested as such; that it reaches the frame is tested by drawing the same
// glyph at pens that snap alike and reading back identical pixels, and at
// pens that snap apart and reading back the difference.
//
// The rendering tests self-skip without a GPU device or a resolvable font.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
constexpr auto snapViewWidth = 96.f;
constexpr auto snapViewHeight = 64.f;

bool fontAvailable()
{
    auto request = FontRequest {};
    request.family = defaultMonospaceFamily();
    request.pointSize = 24.f;

    return GlyphRasterizer {request}.isValid();
}

// Draws one "H" with its pen where the test puts it, at whatever scale the
// view is rendered at.
struct SnapView final : GPU::GPUView
{
    SnapView()
    {
        setSampleCount(1);
        setBounds({0.f, 0.f, snapViewWidth, snapViewHeight});
    }

    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass(
            {{background.r, background.g, background.b, background.a}});

        text.setViewport({snapViewWidth, snapViewHeight}, backingScale());
        text.begin();
        text.draw("H", pen, colour);
        text.flush(pass);
    }

    TextRenderer text {24.f, defaultMonospaceFamily()};
    Graphics::Point pen {8.f, 40.f};
    Graphics::Color background = Graphics::Color::black();
    Graphics::Color colour = Graphics::Color::white();
};

std::optional<Graphics::Image>
    render(Graphics::Point pen, float scale = 1.f, bool darkOnLight = false)
{
    if (!GPU::Device::shared().isValid() || !fontAvailable())
        return std::nullopt;

    auto view = SnapView {};
    view.pen = pen;

    if (darkOnLight)
    {
        view.background = Graphics::Color::white();
        view.colour = Graphics::Color::black();
    }

    auto image = view.renderToImage(scale);

    if (!image.isValid())
        return std::nullopt;

    return image;
}

// How far the pixels moved from the background towards the text.
float ink(const Graphics::Image& image, bool darkOnLight = false)
{
    auto total = 0.f;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            total += darkOnLight ? 1.f - image.at(x, y).r : image.at(x, y).r;

    return total;
}

// Whether `b` is `a` moved down by `rows` pixels, the rows that scrolled in
// being black; with no rows, whether the two are the same picture.
bool sameShiftedDown(const Graphics::Image& a,
                     const Graphics::Image& b,
                     int rows = 0)
{
    if (a.width() != b.width() || a.height() != b.height())
        return false;

    for (auto y = 0; y < b.height(); ++y)
        for (auto x = 0; x < b.width(); ++x)
        {
            const auto expected = y - rows >= 0 ? a.at(x, y - rows).r : 0.f;

            if (std::abs(expected - b.at(x, y).r) > 0.02f)
                return false;
        }

    return true;
}
} // namespace

auto tSnapPenRoundsToPixelsAndPhases =
    test("TextRenderer/snapPenRoundsToPixelsAndPhases") = []
{
    auto snapped = snapPen({10.f, 20.f}, 1.f);
    check(snapped.x == 10 && snapped.y == 20 && snapped.phase == 0);

    // A third of a pixel is nearest the quarter; six tenths the half.
    snapped = snapPen({10.3f, 20.6f}, 1.f);
    check(snapped.x == 10 && snapped.phase == 1);
    check(snapped.y == 21);

    snapped = snapPen({10.6f, 20.4f}, 1.f);
    check(snapped.x == 10 && snapped.phase == 2);
    check(snapped.y == 20);

    // Nine tenths rounds up to the next pixel's own phase, never to a fifth.
    snapped = snapPen({10.9f, 0.f}, 1.f);
    check(snapped.x == 11 && snapped.phase == 0);

    // A negative pen falls in the pixel below it, the way a positive one does.
    snapped = snapPen({-0.3f, -0.6f}, 1.f);
    check(snapped.x == -1 && snapped.phase == 3);
    check(snapped.y == -1);
};

// The snap is in device pixels, not points: at 2x a point is two pixels, so
// 10.3 points is pixel 20 and six tenths of it.
auto tSnapPenWorksInDevicePixels =
    test("TextRenderer/snapPenWorksInDevicePixels") = []
{
    const auto snapped = snapPen({10.3f, 20.6f}, 2.f);

    check(snapped.x == 20 && snapped.phase == 2);
    check(snapped.y == 41);
};

// A recorded glyph names its pen and its glyph rather than texels, so the
// atlas can be asked for the right phase once the pen is final -- and a
// recording made at one scale still draws after the atlas was rebuilt for
// another.
auto tRecordedGlyphNamesItsPen =
    test("TextRenderer/recordedGlyphNamesItsPenAndGlyph") = []
{
    if (!fontAvailable())
        return;

    auto text = TextRenderer {24.f, defaultMonospaceFamily()};
    text.setViewport({100.f, 100.f}, 2.f);

    auto glyphs = Vector<PlacedGlyph> {};
    const auto advance =
        text.layoutInto(glyphs, "AB", {10.3f, 20.6f}, text.getFont());

    check(advance > 0.f);
    check(glyphs.size() == 2);
    check(glyphs[0].pen.x == 10.3f);
    check(glyphs[0].pen.y == 20.6f);
    check(glyphs[1].pen.x > glyphs[0].pen.x);
    check(glyphs[0].face == 0);
    check(!glyphs[0].colored);
    check(glyphs[0].destination.w > 0.f);
    check(glyphs[0].destination.x >= glyphs[0].pen.x - 2.f);
    check(glyphs[0].destination.bottom() > glyphs[0].destination.y);
};

auto tPensThatSnapAlikeDrawAlike =
    test("TextRenderer/pensThatSnapAlikeDrawAlike") = []
{
    const auto a = render({8.f, 40.f});
    const auto b = render({8.1f, 40.4f});

    if (!a || !b)
        return;

    check(ink(*a) > 1.f);
    check(sameShiftedDown(*a, *b));
};

// Six tenths of a pixel down is the next pixel row, exactly: the picture
// moves by a whole row and is otherwise the same, blurred by nothing.
auto tFractionalBaselineLandsOnARow =
    test("TextRenderer/fractionalBaselineLandsOnAWholeRow") = []
{
    const auto a = render({8.f, 40.f});
    const auto b = render({8.f, 40.6f});

    if (!a || !b)
        return;

    check(!sameShiftedDown(*a, *b));
    check(sameShiftedDown(*a, *b, 1));
};

// Half a pixel along the line is a glyph of its own -- the same outline
// rasterized at the other phase, so the ink is the same and the pixels are
// not.
auto tQuarterPixelsAreGlyphsOfTheirOwn =
    test("TextRenderer/halfAPixelAlongTheLineIsAnotherPhase") = []
{
    const auto a = render({8.f, 40.f});
    const auto b = render({8.5f, 40.f});

    if (!a || !b)
        return;

    check(!sameShiftedDown(*a, *b));
    check(std::abs(ink(*a) - ink(*b)) < ink(*a) * 0.15f);
};

// At 2x the snap is to device rows: 40.2 points is row 80.4, which is row 80
// like 40.0 is, and 40.3 points is row 80.6, which is row 81.
auto tSnapsAtTheDeviceScale = test("TextRenderer/snapsToDeviceRowsAtTwoX") = []
{
    const auto a = render({8.f, 40.f}, 2.f);
    const auto same = render({8.f, 40.2f}, 2.f);
    const auto next = render({8.f, 40.3f}, 2.f);

    if (!a || !same || !next)
        return;

    check(a->width() == (int) snapViewWidth * 2);
    check(sameShiftedDown(*a, *same));
    check(sameShiftedDown(*a, *next, 1));
};

// The platform thickens light text more than dark, so the same glyph in
// white on black has at least the ink of black on white -- and on Apple
// visibly more, the mask for each having been drawn in its own lightness.
// The blend itself is symmetric, so the difference is the masks'.
auto tDarkTextIsNoHeavier =
    test("TextRenderer/darkTextOnLightIsNoHeavierThanLightOnDark") = []
{
    const auto light = render({8.f, 40.f});
    const auto dark = render({8.f, 40.f}, 1.f, true);

    if (!light || !dark)
        return;

    const auto lightInk = ink(*light);
    const auto darkInk = ink(*dark, true);

    check(darkInk > 1.f);
    check(lightInk >= darkInk * 0.98f);

#if defined(__APPLE__)
    check(lightInk > darkInk * 1.03f);
#endif
};
