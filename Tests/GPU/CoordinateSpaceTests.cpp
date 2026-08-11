#include "Common.h"

#include <eacp/Sprites/Sprites.h>

#include <optional>

// Regression: Rect's splitters were y-up while the shaders, setScissorRect and
// the backing views are y-down, so removeFromTop() returned the bottom slice.

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

    // Must outlive render(): a local would release its buffers while the command
    // list recording the draws is still unsubmitted, and D3D12 then draws nothing.
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

    // Before the fix this row was green.
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

    // Transposed vertical splitters would put the black gap against an edge.
    check(isBlue(image.at(4, image.height() / 2)));
    check(isBlack(image.at(middle, image.height() / 2)));

    check(isRed(image.at(4, 2)));
    check(isGreen(image.at(4, image.height() - 3)));
};

auto tScissorSharesTheConvention =
    test("CoordinateSpace/scissorClipsTheSameWayUp") = []
{
    // setScissorRect is documented as top-left origin in pixels, like Rect.
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

            sprites->setScissorRect({top.x, top.y, top.w, top.h});
            sprites->fillRect({0.f, 0.f, viewW, viewH}, topColor);
            sprites->clearScissorRect();
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
