#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>

// The blurred rounded box the shape batch draws: what a box-shadow is, and the
// only shape in the tier whose edge is wider than a pixel -- and the only one
// that is not drawn where it was asked for, a shadow being cast as though the
// box casting it were opaque.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
const auto black = Color {0.f, 0.f, 0.f, 1.f};
const auto card = Rect {50.f, 50.f, 100.f, 100.f};

struct Shadow final : Component
{
    void paint(UI::Graphics& g) override
    {
        g.setColour(black);
        g.drawShadow(shadow);
    }

    ShadowShape shadow;
};

struct RoundedFill final : Component
{
    void paint(UI::Graphics& g) override
    {
        g.setColour(black);
        g.fillRoundedRect(rect, cornerRadius);
    }

    Rect rect;
    float cornerRadius = 0.f;
};

eacp::Graphics::Image renderOne(Component& content)
{
    auto host = ComponentHost {};

    host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
    host.setBounds({0.f, 0.f, 200.f, 200.f});
    host.resized();
    host.setRootComponent(content);

    return host.renderToImage(1.f);
}
} // namespace

auto tShadowFades = test("Shadow/aBlurredBoxFadesOverTwiceItsBlur") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto content = Shadow {};
    content.shadow = {card, 0.f, {}, 20.f};

    auto image = renderOne(content);
    auto alphaAt = [&](int x, int y) { return image.at(x, y).a; };

    // Nothing a blur outside the box, which is what the quad was grown by,
    // and nothing inside it, which is where a shadow is never drawn.
    check(alphaAt(100, 29) < 0.03f, "nothing a blur outside the edge");
    check(alphaAt(100, 100) < 0.02f, "nor behind the box that cast it");

    // Just outside the edge, most of it: the ramp is centred on the edge, so
    // half the shadow is under the box and half of it shows.
    check(alphaAt(100, 48) > 0.4f, "and most of it just outside");

    // Monotonic across the ramp: the fade is a curve, not a stack of bands.
    for (auto y = 31; y < 44; y += 4)
        check(alphaAt(100, y) < alphaAt(100, y + 4), "rising all the way");
};

auto tShadowSpreadsFromItsCorners =
    test("Shadow/aBlurredBoxIsFainterPastACornerThanPastAnEdge") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto content = Shadow {};
    content.shadow = {card, 0.f, {}, 20.f};

    auto image = renderOne(content);

    // The same distance from the box, but diagonally out of a corner: less of
    // the box is near it, so less of the shadow reaches.
    auto pastEdge = image.at(100, 40).a;
    auto pastCorner = image.at(43, 43).a;

    check(pastCorner < pastEdge, "a corner casts less than an edge");
    check(pastCorner > 0.f, "and still casts something");
};

auto tSharpShadowIsAFill = test("Shadow/aShadowWithNoBlurIsTheFillItSpreadsTo") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto content = Shadow {};
    content.shadow = {card, 8.f, {}, 0.f, 20.f};

    auto fill = RoundedFill {};
    fill.rect = {30.f, 30.f, 140.f, 140.f};
    fill.cornerRadius = 28.f;

    auto shadowImage = renderOne(content);
    auto fillImage = renderOne(fill);

    for (auto y = 20; y < 180; y += 3)
    {
        for (auto x = 20; x < 180; x += 3)
        {
            // Everywhere but the box itself, where the shadow is not drawn
            // and the fill is.
            if (x > 46 && x < 154 && y > 46 && y < 154)
                continue;

            auto difference =
                std::abs(shadowImage.at(x, y).a - fillImage.at(x, y).a);

            check(difference < 0.02f, "every pixel, corners included");
        }
    }
};

auto tShadowFallsWhereItIsThrown =
    test("Shadow/anOffsetShadowLeavesTheBoxBehind") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto content = Shadow {};
    content.shadow = {card, 0.f, {0.f, 20.f}, 8.f};

    auto image = renderOne(content);

    check(image.at(100, 160).a > 0.8f, "below the box, where it fell");
    check(image.at(100, 40).a < 0.02f, "and nothing above it");
};

auto tInsetShadow = test("Shadow/anInsetShadowIsDrawnInsideTheBoxAlone") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto content = Shadow {};
    content.shadow = {card, 0.f, {}, 10.f, 0.f, true};

    auto image = renderOne(content);

    check(image.at(100, 45).a < 0.02f, "nothing outside the box");
    check(image.at(100, 51).a > 0.35f, "dark just inside its edge");
    check(image.at(100, 51).a > image.at(100, 55).a, "fading inwards");
    check(image.at(100, 100).a < 0.03f, "and gone by the middle");
};
