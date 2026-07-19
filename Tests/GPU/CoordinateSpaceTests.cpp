#include "Common.h"

#include <eacp/Sprites/Sprites.h>

#include <optional>

// The contract between Graphics::Rect and the things that draw it: a rect taken
// from the top of an area must appear at the top of the image.
//
// Nothing checked this before, and the two halves disagreed for a long time --
// Rect's splitters were y-up (inherited from JUCE, where views are y-up) while
// the sprite shader, the glyph shader, setScissorRect and the isFlipped backing
// views are all y-down. So `area.removeFromTop(h)` returned the bottom slice,
// and every app that laid out a header with it put the header along the bottom.
//
// Unit tests on Rect alone cannot catch that: they can only confirm the
// arithmetic agrees with whatever convention the test author had in mind. It
// takes rendering to say which end of the screen a rect actually lands on,
// which is what this does -- fill the slices with telltale colours, read the
// pixels back, assert on where they came out.
//
// Self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewW = 120.f;
constexpr auto viewH = 80.f;

constexpr auto topColor = Graphics::Color {1.f, 0.f, 0.f};
constexpr auto bottomColor = Graphics::Color {0.f, 1.f, 0.f};
constexpr auto leftColor = Graphics::Color {0.f, 0.f, 1.f};

// Splits its bounds the way an app lays out chrome, and fills each slice with a
// colour that says which splitter produced it.
struct ChromeView final : GPUView
{
    ChromeView()
    {
        setSampleCount(1);
        setBounds({0.f, 0.f, viewW, viewH});
    }

    void render(Frame& frame) override
    {
        if (!sprites)
            sprites.emplace(Graphics::Point {viewW, viewH}, sampleCount());

        auto pass = frame.beginPass({Graphics::Color::black()});
        sprites->begin(pass);

        auto area = Graphics::Rect {0.f, 0.f, viewW, viewH};

        const auto top = area.removeFromTop(16.f);
        const auto bottom = area.removeFromBottom(16.f);
        const auto left = area.removeFromLeft(24.f);

        sprites->fillRect(top, topColor);
        sprites->fillRect(bottom, bottomColor);
        sprites->fillRect(left, leftColor);
    }

    // Outlives render(), like every app that draws with one. A renderer built as
    // a local here would release its vertex buffer and pipeline at the end of
    // render(), while the command list recording those draws is still waiting to
    // be submitted -- on D3D12 the frame then draws nothing at all.
    std::optional<Sprites::SpriteRenderer> sprites;
};

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f && c.b < 0.5f;
}

bool isBlue(const Graphics::Color& c)
{
    return c.b > 0.5f && c.r < 0.5f && c.g < 0.5f;
}

bool isBlack(const Graphics::Color& c)
{
    return c.r < 0.2f && c.g < 0.2f && c.b < 0.2f;
}
} // namespace

auto tRemoveFromTopDrawsAtTheTop =
    test("CoordinateSpace/removeFromTopDrawsAtTheTop") = []
{
    auto view = ChromeView {};
    auto image = view.renderToImage(1.f);

    if (image.width() == 0)
        return;

    const auto middle = image.width() / 2;

    // Row 0 is the top of the image, so the slice taken from the top has to be
    // there. Before the fix this row was green.
    check(isRed(image.at(middle, 2)));
    check(isGreen(image.at(middle, image.height() - 3)));
};

auto tSlicesTileTheImage = test("CoordinateSpace/slicesTileWithoutOverlap") = []
{
    auto view = ChromeView {};
    auto image = view.renderToImage(1.f);

    if (image.width() == 0)
        return;

    const auto middle = image.width() / 2;

    // Between the two bars: the left column, then the untouched middle. If the
    // vertical splitters ran the wrong way these bands would be transposed and
    // the black gap would sit against an edge instead of in the middle.
    check(isBlue(image.at(4, image.height() / 2)));
    check(isBlack(image.at(middle, image.height() / 2)));

    // The bars span the full width, including over the left column's x range.
    check(isRed(image.at(4, 2)));
    check(isGreen(image.at(4, image.height() - 3)));
};

auto tScissorSharesTheConvention =
    test("CoordinateSpace/scissorClipsTheSameWayUp") = []
{
    // setScissorRect is documented as top-left origin in pixels. If it and Rect
    // disagreed, clipping a widget to its own bounds would clip the wrong end.
    struct ClippedView final : GPUView
    {
        ClippedView()
        {
            setSampleCount(1);
            setBounds({0.f, 0.f, viewW, viewH});
        }

        void render(Frame& frame) override
        {
            if (!sprites)
                sprites.emplace(Graphics::Point {viewW, viewH}, sampleCount());

            auto pass = frame.beginPass({Graphics::Color::black()});
            sprites->begin(pass);

            auto area = Graphics::Rect {0.f, 0.f, viewW, viewH};
            const auto top = area.removeFromTop(16.f);

            // Clip to the top slice, then fill the whole view. Only the top
            // slice should survive.
            pass.setScissorRect({top.x, top.y, top.w, top.h});
            sprites->fillRect({0.f, 0.f, viewW, viewH}, topColor);
            pass.clearScissorRect();
        }

        std::optional<Sprites::SpriteRenderer> sprites;
    };

    auto view = ClippedView {};
    auto image = view.renderToImage(1.f);

    if (image.width() == 0)
        return;

    const auto middle = image.width() / 2;

    check(isRed(image.at(middle, 2)));
    check(isBlack(image.at(middle, image.height() / 2)));
    check(isBlack(image.at(middle, image.height() - 3)));
};
