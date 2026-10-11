#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>

// A fill under a clip rectangle that does not start at the origin: the case a
// letterboxed SVG hits, where every clip in the document is moved down by the
// letterbox and the shapes under it are cut by a rectangle rather than a mask.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
const auto red = Color {1.f, 0.f, 0.f, 1.f};
const auto blue = Color {0.f, 0.f, 1.f, 1.f};

bool isNear(float a, float b, float tolerance = 0.02f)
{
    return std::abs(a - b) <= tolerance;
}

bool isColour(const Color& actual, const Color& expected)
{
    return isNear(actual.r, expected.r) && isNear(actual.g, expected.g)
           && isNear(actual.b, expected.b) && actual.a > 0.9f;
}

struct ClippedFills final : Component
{
    // From resized() rather than paint(): a path set while painting is
    // rasterized for the frame after, and this test renders one.
    void resized() override
    {
        auto path = GPUWidgets::Path {};
        path.addRect({150.f, 51.43f, 150.f, 47.14f});
        shape.setBacking(PathShape::Backing::Mask);
        shape.setPath(path);
    }

    void paint(UI::Graphics& g) override
    {
        {
            auto scope = UI::Graphics::ScopedState {g};
            g.reduceClipRegion({0.f, 51.43f, 150.f, 45.9f});
            g.setColour(red);
            g.fillRect({0.f, 51.43f, 150.f, 47.14f});
        }

        {
            auto scope = UI::Graphics::ScopedState {g};
            g.reduceClipRegion({150.f, 51.43f, 150.f, 45.9f});
            g.setColour(blue);
            g.fillPath(shape);
        }
    }

    PathShape shape {*this};
};

struct Overhang final : Component
{
    void paint(UI::Graphics& g) override
    {
        g.setColour(red);
        g.fillRect({-20.f, 0.f, getWidth() + 40.f, getHeight()});
    }
};

struct OverhangParent final : Component
{
    OverhangParent() { addAndMakeVisible(child); }

    void resized() override { child.setBounds({100.f, 50.f, 100.f, 50.f}); }

    Overhang child;
};
} // namespace

auto tClippedFills = test("ClippedFill/aFillUnderAClipAwayFromTheOriginDraws") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto host = ComponentHost {};
    auto fills = ClippedFills {};

    host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
    host.setBounds({0.f, 0.f, 300.f, 150.f});
    host.resized();
    host.setRootComponent(fills);

    for (auto scale: {host.backingScale(), 1.f})
    {
        auto image = host.renderToImage(scale);
        auto at = [&](float x, float y)
        { return image.at((int) (x * scale), (int) (y * scale)); };

        check(isColour(at(75.f, 75.f), red), "a rect fill under the moved clip");
        check(isColour(at(225.f, 75.f), blue), "a mask fill under the moved clip");
        check(at(75.f, 30.f).a < 0.1f, "and nothing above the clip");
        check(at(225.f, 30.f).a < 0.1f);
    }
};

auto tUnclipped = test("ClippedFill/anUnclippedComponentPaintsPastItsBounds") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto host = ComponentHost {};
    auto parent = OverhangParent {};

    host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
    host.setBounds({0.f, 0.f, 300.f, 150.f});
    host.resized();
    host.setRootComponent(parent);
    parent.setBounds({0.f, 0.f, 300.f, 150.f});

    auto at = [](const auto& image, float x, float y)
    { return image.at((int) x, (int) y); };

    auto clipped = host.renderToImage(1.f);

    check(isColour(at(clipped, 150.f, 75.f), red));
    check(at(clipped, 90.f, 75.f).a < 0.1f, "cut at its bounds by default");

    parent.child.setPaintingIsUnclipped(true);
    auto unclipped = host.renderToImage(1.f);

    check(isColour(at(unclipped, 90.f, 75.f), red), "past its left edge");
    check(isColour(at(unclipped, 210.f, 75.f), red), "and its right");
    check(at(unclipped, 150.f, 30.f).a < 0.1f, "and nowhere it did not paint");
};
