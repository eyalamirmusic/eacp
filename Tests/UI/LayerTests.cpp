#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// A layer's contract with the component that owns it and the host that renders
// it, which is the part nothing on screen would report.
//
// It finds its way into a frame the way a PathShape does: by registering with
// its component in its constructor, so the host can walk the tree and fill every
// one of them before the frame's own pass opens. Miss that and the layer is
// simply never rendered -- there is no texture to draw, so the group it holds is
// absent and nothing says why.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

auto tLayerRegisters = test("Layer/aLayerRegistersWithItsComponent") = []
{
    auto component = Component {};

    check(component.getLayers().size() == 0);

    {
        auto layer = Layer {component};

        check(component.getLayers().size() == 1);
        check(component.getLayers()[0] == &layer);
    }

    check(component.getLayers().size() == 0, "and is gone with it");
};

auto tLayerStartsEmpty = test("Layer/aLayerHasNothingInItUntilItIsRendered") = []
{
    auto component = Component {};
    auto layer = Layer {component};

    check(layer.isEmpty(), "so drawing it is a no-op rather than a black square");

    layer.setBounds({10.f, 20.f, 30.f, 40.f});

    check(sameRect(layer.getBounds(), {10.f, 20.f, 30.f, 40.f}));
    check(layer.isEmpty());
};

// The fade is applied where the layer is composited, so changing it is a repaint
// and not a re-render -- which is what makes an animated opacity affordable.
auto tOpacityDoesNotDirty = test("Layer/fadingALayerDoesNotRebuildIt") = []
{
    auto component = Component {};
    auto layer = Layer {component};

    layer.setOpacity(0.5f);

    check(layer.getOpacity() == 0.5f);

    layer.setOpacity(2.f);

    check(layer.getOpacity() == 1.f, "clamped, since it multiplies a colour");

    layer.setOpacity(-1.f);

    check(layer.getOpacity() == 0.f);
};

// Bounds it already has are not a change, because setting them asks for the
// content again and a layer re-rendered every frame for nothing is a render pass
// per frame for nothing.
auto tSameBoundsAreNotAChange = test("Layer/settingTheSameBoundsAsksForNothing") = []
{
    auto component = Component {};
    auto layer = Layer {component};

    layer.setBounds({0.f, 0.f, 50.f, 50.f});
    layer.setBounds({0.f, 0.f, 50.f, 50.f});

    check(sameRect(layer.getBounds(), {0.f, 0.f, 50.f, 50.f}));
};

// And the transform is the other one that is not: what the texture holds is the
// content as it was drawn, and the matrix places the quad it is composited as.
auto tTransformStartsAsIdentity = test("Layer/aLayerStartsUnturned") = []
{
    auto component = Component {};
    auto layer = Layer {component};

    check(layer.getTransform().isIdentity());

    layer.setTransform(GPUWidgets::AffineTransform::scaling(2.f, 2.f));

    check(layer.getTransform().a == 2.f);
    check(layer.getBounds().isEmpty(), "which the bounds know nothing about");
};

namespace
{
// A layer of one flat colour, so where it lands is the only thing a readback is
// reporting.
struct Square final : Component
{
    Square()
        : layer(*this)
    {
        layer.onPaint = [this](UI::Graphics& g)
        {
            g.setColour(Color {1.f, 0.f, 0.f, 1.f});
            g.fillRect(layer.getBounds());
        };
    }

    void paint(UI::Graphics& g) override { g.drawLayer(layer); }

    Layer layer;
};

eacp::Graphics::Image renderSquare(const Rect& bounds,
                                   const GPUWidgets::AffineTransform& transform)
{
    auto content = Square {};
    content.layer.setBounds(bounds);
    content.layer.setTransform(transform);

    auto host = ComponentHost {};

    host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
    host.setBounds({0.f, 0.f, 200.f, 200.f});
    host.resized();
    host.setRootComponent(content);

    return host.renderToImage(1.f);
}

bool isRed(const eacp::Graphics::Image& image, int x, int y)
{
    auto pixel = image.at(x, y);
    return pixel.a > 0.9f && pixel.r > 0.9f;
}

bool isEmpty(const eacp::Graphics::Image& image, int x, int y)
{
    return image.at(x, y).a < 0.1f;
}
} // namespace

auto tUnturnedLayerLandsInItsBounds =
    test("Layer/anUnturnedLayerFillsTheBoundsItWasGiven") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto image = renderSquare({50.f, 50.f, 50.f, 50.f}, {});

    check(isRed(image, 75, 75), "inside");
    check(isEmpty(image, 120, 75), "and nothing past its right edge");
};

// A tree recorded by hand before its first frame - paintDirtyComponents,
// which is what a headless snapshot and a test do - names a layer that has
// no texture yet. The recording has to name it all the same: the frame
// renders the layer and then plays the recording, and nothing would record
// the component again for the layer's sake.
auto tLayerRecordedBeforeItIsRendered =
    test("Layer/aLayerRecordedBeforeItsFirstFrameIsStillDrawn") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto content = Square {};
    content.layer.setBounds({50.f, 50.f, 50.f, 50.f});

    auto host = ComponentHost {};

    host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
    host.setBounds({0.f, 0.f, 200.f, 200.f});
    host.resized();
    host.setRootComponent(content);

    // A first frame at the scale the rest is judged at, so a later one has
    // no reason of its own to record the tree again.
    check(isRed(host.renderToImage(1.f), 75, 75));

    // The layer moved, which empties it, and the tree recorded by hand
    // before the frame that fills it.
    content.layer.setBounds({100.f, 50.f, 50.f, 50.f});

    check(host.paintDirtyComponents() > 0, "recorded before the frame");
    check(content.layer.isEmpty(), "with the layer not yet rendered");
    check(!content.needsRepaint(), "and nothing left to record");

    auto image = host.renderToImage(1.f);

    check(isRed(image, 125, 75), "the layer is drawn all the same");
    check(isEmpty(image, 75, 75), "where it now is");
};

// The matrix is in the layer's own space, so this moves the content 60 to the
// right of wherever the bounds put it -- and a caller that wanted it moved
// would have set the bounds.
auto tTranslatedLayerMoves = test("Layer/aTranslatedLayerDrawsWhereItWasSent") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto image = renderSquare({50.f, 50.f, 50.f, 50.f},
                              GPUWidgets::AffineTransform::translation(60.f, 0.f));

    check(isRed(image, 135, 75), "60 to the right");
    check(isEmpty(image, 75, 75), "and gone from where it was");
};

// About the layer's own top-left rather than the component's, which is the
// difference that lets a layer inside another one composite the same wherever
// the outer one put it.
auto tScaledLayerGrowsFromItsOwnOrigin =
    test("Layer/aScaledLayerGrowsFromItsOwnCorner") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto image = renderSquare({50.f, 50.f, 50.f, 50.f},
                              GPUWidgets::AffineTransform::scaling(2.f, 2.f));

    check(isRed(image, 55, 55), "still anchored where the bounds are");
    check(isRed(image, 140, 140), "and twice as far across");
    check(isEmpty(image, 160, 160), "and no further");
};

auto tRotatedLayerTurns = test("Layer/aRotatedLayerIsCutByAnUprightScissor") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    // A quarter turn about the middle of a 60-point square at (70, 70) leaves
    // it where it was, being square -- so what this reports is that a rotation
    // composites at all, and where a corner of it went.
    auto centred = GPUWidgets::AffineTransform::rotationAbout(
        3.14159265f / 4.f, {30.f, 30.f});

    auto image = renderSquare({70.f, 70.f, 60.f, 60.f}, centred);

    check(isRed(image, 100, 100), "the middle stays put");
    check(isRed(image, 100, 65), "and a corner comes round to the top");
    check(isEmpty(image, 75, 75), "leaving the old corner behind");
};
