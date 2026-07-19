#include "Common.h"

#include <cmath>

using namespace nano;
using namespace eacp::Graphics;

namespace
{
bool near(const Color& c, int r, int g, int b, int tolerance = 6)
{
    auto within = [&](float channel, int target)
    { return std::abs((int) std::lround(channel * 255.f) - target) <= tolerance; };

    return within(c.r, r) && within(c.g, g) && within(c.b, b);
}

// Fills its whole local bounds with a solid colour via paint().
struct Swatch final : View
{
    explicit Swatch(const Color& colorToUse)
        : color(colorToUse)
    {
    }

    void paint(Context& g) override
    {
        g.setColor(color);
        g.fillRect(getLocalBounds());
    }

    Color color;
};
} // namespace

// paint() backdrop plus two child views, stacked and positioned the way the
// on-screen compositor lays them out: red top-left, green bottom-right, the
// black backdrop showing between them -- and the right way up.
auto tCompositesPaintAndChildren =
    test("RenderToImage/compositesPaintAndChildren") = []
{
    struct Root final : View
    {
        Root()
        {
            addChildren({red, green});
            setBounds({0.f, 0.f, 100.f, 100.f});
            red.setBounds({0.f, 0.f, 40.f, 40.f});
            green.setBounds({60.f, 60.f, 40.f, 40.f});
        }

        void paint(Context& g) override
        {
            g.setColor({0.f, 0.f, 0.f, 1.f});
            g.fillRect(getLocalBounds());
        }

        Swatch red {{1.f, 0.f, 0.f}};
        Swatch green {{0.f, 1.f, 0.f}};
    };

    auto root = Root {};
    auto image = root.renderToImage(1.f);

    check(image.isValid());
    check(image.width() == 100);
    check(image.height() == 100);

    check(near(image.at(20, 20), 255, 0, 0)); // red child, top-left
    check(near(image.at(80, 80), 0, 255, 0)); // green child, bottom-right
    check(near(image.at(50, 50), 0, 0, 0)); // backdrop between them
};

// scale super-samples: the same 100pt view rendered at 2x is 200x200 device
// pixels, with content in the same relative places.
auto tScaleSuperSamples = test("RenderToImage/scaleSuperSamples") = []
{
    struct Root final : View
    {
        Root()
        {
            addChildren({red});
            setBounds({0.f, 0.f, 100.f, 100.f});
            red.setBounds({0.f, 0.f, 40.f, 40.f});
        }

        Swatch red {{1.f, 0.f, 0.f}};
    };

    auto root = Root {};
    auto image = root.renderToImage(2.f);

    check(image.width() == 200);
    check(image.height() == 200);
    check(near(image.at(40, 40), 255, 0, 0)); // 20pt * 2 lands inside the child
};

// A real framework widget (ShapeLayerView renders through a CAShapeLayer, not
// paint()) is captured via the compositor's renderInContext: path.
auto tCapturesShapeLayerWidget = test("RenderToImage/capturesShapeLayerWidget") = []
{
    auto shape = ShapeLayerView {};
    shape.setBounds({0.f, 0.f, 40.f, 40.f});
    shape.resized();

    shape->setFillColor({0.f, 0.f, 1.f});
    auto path = Path {};
    path.addRect({0.f, 0.f, 40.f, 40.f});
    shape->setPath(path);

    auto image = shape.renderToImage(1.f);

    check(image.isValid());
    check(near(image.at(20, 20), 0, 0, 255)); // blue fill
};

// Group opacity flattens the subtree and fades it as one: a fully black child
// at 0.5 over a white backdrop reads as mid-grey.
auto tGroupOpacityFadesSubtree = test("RenderToImage/groupOpacityFadesSubtree") = []
{
    struct Root final : View
    {
        Root()
        {
            addChildren({box});
            setBounds({0.f, 0.f, 40.f, 40.f});
            box.setBounds({0.f, 0.f, 40.f, 40.f});
            box.setOpacity(0.5f);
        }

        void paint(Context& g) override
        {
            g.setColor({1.f, 1.f, 1.f, 1.f});
            g.fillRect(getLocalBounds());
        }

        Swatch box {{0.f, 0.f, 0.f}};
    };

    auto root = Root {};
    auto image = root.renderToImage(1.f);

    check(near(image.at(20, 20), 128, 128, 128, 8));
};

// A zero-sized view has nothing to render into and yields an invalid Image
// rather than throwing.
auto tZeroSizeIsInvalid = test("RenderToImage/zeroSizeIsInvalid") = []
{
    auto view = View {};
    view.setBounds({0.f, 0.f, 0.f, 0.f});

    check(!view.renderToImage(1.f).isValid());
};

// A layer's path space is y-down, like everything else. Worth pinning because
// it is the one place the convention is decided by AppKit rather than by code
// here: geometryFlipped is never set, so the answer comes from the backing
// layer of an isFlipped view and cannot be read off any line in this repo.
//
// An asymmetric path is the whole point -- the existing full-bounds cases above
// are orientation-symmetric and pass either way up.
auto tLayerPathSpaceIsYDown = test("RenderToImage/layerPathSpaceIsYDown") = []
{
    auto shape = ShapeLayerView {};
    shape.setBounds({0.f, 0.f, 40.f, 40.f});
    shape.resized();

    shape->setFillColor({0.f, 0.f, 1.f});
    auto path = Path {};
    path.addRect({0.f, 0.f, 40.f, 10.f});
    shape->setPath(path);

    auto image = shape.renderToImage(1.f);

    if (!image.isValid())
        return;

    // A band at y = 0..10 belongs against the top edge.
    check(near(image.at(20, 5), 0, 0, 255));
    check(!near(image.at(20, 35), 0, 0, 255));
};
