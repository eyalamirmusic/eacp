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

    check(near(image.at(20, 20), 255, 0, 0));
    check(near(image.at(80, 80), 0, 255, 0));
    check(near(image.at(50, 50), 0, 0, 0));
};

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
    check(near(image.at(20, 20), 0, 0, 255));
};

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

auto tZeroSizeIsInvalid = test("RenderToImage/zeroSizeIsInvalid") = []
{
    auto view = View {};
    view.setBounds({0.f, 0.f, 0.f, 0.f});

    check(!view.renderToImage(1.f).isValid());
};

// Nothing in this repo decides this: geometryFlipped is never set, so a layer's
// y-down path space comes from the backing layer of an isFlipped view.
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

    check(near(image.at(20, 5), 0, 0, 255));
    check(!near(image.at(20, 35), 0, 0, 255));
};
