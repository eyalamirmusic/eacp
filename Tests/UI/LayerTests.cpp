#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <memory>

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

namespace LayerTests
{
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

// A layer's drawing may render something else off-screen on the way - a
// picture made the first time it is drawn - which nests a frame and flushes
// the one rendering the layer. The drawing is recorded before the layer's
// pass opens, so that flush finds no encoder open.
auto tLayerDrawingRendersOffScreen =
    test("Layer/aLayerWhoseDrawingRendersOffScreenIsStillDrawn") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    struct Nesting final : Component
    {
        Nesting()
            : layer(*this)
        {
            layer.onPaint = [this](UI::Graphics& g)
            {
                nested = renderSquare({0.f, 0.f, 10.f, 10.f}, {});
                g.setColour(Color {1.f, 0.f, 0.f, 1.f});
                g.fillRect(layer.getBounds());
            };
        }

        void paint(UI::Graphics& g) override { g.drawLayer(layer); }

        Layer layer;
        eacp::Graphics::Image nested;
    };

    auto content = Nesting {};
    content.layer.setBounds({50.f, 50.f, 50.f, 50.f});

    auto host = ComponentHost {};

    host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
    host.setBounds({0.f, 0.f, 200.f, 200.f});
    host.resized();
    host.setRootComponent(content);

    auto image = host.renderToImage(1.f);

    check(isRed(content.nested, 5, 5), "the picture rendered inside");
    check(isRed(image, 75, 75), "and the layer around it");
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
    auto centred =
        GPUWidgets::AffineTransform::rotationAbout(3.14159265f / 4.f, {30.f, 30.f});

    auto image = renderSquare({70.f, 70.f, 60.f, 60.f}, centred);

    check(isRed(image, 100, 100), "the middle stays put");
    check(isRed(image, 100, 65), "and a corner comes round to the top");
    check(isEmpty(image, 75, 75), "leaving the old corner behind");
};

namespace
{
// A square inset from the layer's own corner on a fill of its bounds, so a
// layer rendered at the wrong size shows as the square moved and grown.
struct InsetSquare final : Component
{
    InsetSquare()
        : layer(*this)
    {
        layer.onPaint = [this](UI::Graphics& g)
        {
            auto bounds = layer.getBounds();

            g.setColour(Color {0.f, 0.f, 1.f, 1.f});
            g.fillRect(bounds);
            g.setColour(Color {1.f, 0.f, 0.f, 1.f});
            g.fillRect({bounds.x + 10.f, bounds.y + 10.f, 20.f, 20.f});
        };
    }

    void paint(UI::Graphics& g) override { g.drawLayer(layer); }

    Layer layer;
};

struct LayerHost
{
    LayerHost()
    {
        host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
        host.setBounds({0.f, 0.f, 200.f, 200.f});
        host.resized();
        host.setRootComponent(content);
    }

    eacp::Graphics::Image render(const Rect& bounds, float scale)
    {
        content.layer.setBounds(bounds);
        return host.renderToImage(scale);
    }

    InsetSquare content;
    ComponentHost host;
};

eacp::Graphics::Image renderFresh(const Rect& bounds, float scale)
{
    return LayerHost {}.render(bounds, scale);
}

bool samePixels(const eacp::Graphics::Image& a, const eacp::Graphics::Image& b)
{
    if (a.width() != b.width() || a.height() != b.height())
        return false;

    for (auto y = 0; y < a.height(); ++y)
    {
        for (auto x = 0; x < a.width(); ++x)
        {
            auto p = a.at(x, y);
            auto q = b.at(x, y);

            if (std::abs(p.r - q.r) > 0.02f || std::abs(p.g - q.g) > 0.02f
                || std::abs(p.b - q.b) > 0.02f || std::abs(p.a - q.a) > 0.02f)
                return false;
        }
    }

    return true;
}
} // namespace

// The texture is kept across a shrink, and the content has to land 1:1 in the
// corner of it that is composited -- not be stretched over the whole of it and
// then shown magnified.
auto tShrunkLayerMatchesAFreshOne =
    test("Layer/aShrunkLayerDrawsAsAFreshOneOfItsNewSize") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto wide = Rect {20.f, 20.f, 150.f, 150.f};
    auto narrow = Rect {20.f, 20.f, 60.f, 60.f};

    auto layered = LayerHost {};

    check(samePixels(layered.render(wide, 1.f), renderFresh(wide, 1.f)));

    auto shrunk = layered.render(narrow, 1.f);

    check(isRed(shrunk, 35, 35), "the square where it was drawn");
    check(!isRed(shrunk, 60, 60), "at the size it was drawn");
    check(samePixels(shrunk, renderFresh(narrow, 1.f)), "pixel for pixel");

    check(samePixels(layered.render(wide, 1.f), renderFresh(wide, 1.f)),
          "and grown back again");
};

// Texels are device pixels, so content rendered for one scale is the wrong
// resolution at another and is rendered again rather than resampled.
auto tLayerFollowsTheScale = test("Layer/aLayerIsRenderedAgainAtANewScale") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto wide = Rect {20.f, 20.f, 150.f, 150.f};
    auto narrow = Rect {20.f, 20.f, 60.f, 60.f};

    auto layered = LayerHost {};

    layered.render(wide, 1.f);

    check(samePixels(layered.render(wide, 2.f), renderFresh(wide, 2.f)));
    check(samePixels(layered.render(narrow, 1.f), renderFresh(narrow, 1.f)),
          "and kept the larger texture across the change");
};

namespace
{
const auto red = Color {1.f, 0.f, 0.f, 1.f};
const auto green = Color {0.f, 1.f, 0.f, 1.f};

struct Fill final : Component
{
    void paint(UI::Graphics& g) override
    {
        ++paints;
        g.setColour(colour);
        g.fillRect(getLocalBounds());
    }

    Color colour = red;
    int paints = 0;
};

// A child the layer captures, and a grandchild inside it, so a change deep in
// the subtree has somewhere to happen. The owner composites the layer only when
// asked, so a frame with it off shows what the frame itself drew.
struct Capturing final : Component
{
    Capturing()
        : layer(*this)
    {
        addAndMakeVisible(content);
        content.addAndMakeVisible(inner);

        layer.setContent(&content);
    }

    void resized() override
    {
        content.setBounds({20.f, 20.f, 40.f, 40.f});
        inner.setBounds({10.f, 10.f, 10.f, 10.f});
        layer.setBounds({10.f, 10.f, 60.f, 60.f});
    }

    void paint(UI::Graphics& g) override
    {
        if (composite)
            g.drawLayer(layer);
    }

    Fill content;
    Fill inner;
    Layer layer;
    bool composite = false;
};

struct CapturingHost
{
    CapturingHost()
    {
        host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
        host.setBounds({0.f, 0.f, 200.f, 200.f});
        host.resized();
        host.setRootComponent(owner);
    }

    eacp::Graphics::Image render() { return host.renderToImage(1.f); }

    Capturing owner;
    ComponentHost host;
};

bool isColour(const eacp::Graphics::Image& image, int x, int y, const Color& c)
{
    auto pixel = image.at(x, y);

    return pixel.a > 0.9f && std::abs(pixel.r - c.r) < 0.1f
           && std::abs(pixel.g - c.g) < 0.1f && std::abs(pixel.b - c.b) < 0.1f;
}
} // namespace

auto tContentIsCapturedNotDrawn =
    test("Layer/aCapturedSubtreeGoesIntoTheLayerAndNotTheFrame") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto fixture = CapturingHost {};

    check(fixture.owner.layer.getContent() == &fixture.owner.content);

    auto uncomposited = fixture.render();

    check(isEmpty(uncomposited, 40, 40), "the frame steps over the subtree");
    check(!fixture.owner.layer.isEmpty(), "and the layer holds it");
    check(fixture.host.getLastRenderedLayerCount() == 1);

    fixture.owner.composite = true;
    fixture.owner.repaint();

    auto composited = fixture.render();

    check(isColour(composited, 25, 25, red), "where the content sits");
    check(isEmpty(composited, 15, 15), "and not the rest of the bounds");
    check(isEmpty(composited, 65, 65), "nor past the content");

    // Moved by the composite rather than by the content, so what is drawn there
    // can only have come out of the texture.
    fixture.owner.layer.setTransform(
        GPUWidgets::AffineTransform::translation(100.f, 0.f));

    auto moved = fixture.render();

    check(isColour(moved, 125, 25, red), "the texture, 100 to the right");
    check(isEmpty(moved, 25, 25), "and nothing left behind");
};

auto tCapturedChangeDirtiesTheLayer =
    test("Layer/aChangeInsideTheCapturedSubtreeRendersTheLayerAgain") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto fixture = CapturingHost {};
    fixture.owner.composite = true;

    fixture.render();
    fixture.render();

    check(fixture.host.getLastRenderedLayerCount() == 0, "nothing changed");

    fixture.owner.inner.colour = green;
    fixture.owner.inner.repaint();

    auto repainted = fixture.render();

    check(fixture.host.getLastRenderedLayerCount() == 1, "a repaint deep inside");
    check(isColour(repainted, 35, 35, green), "and the layer shows it");

    fixture.render();
    check(fixture.host.getLastRenderedLayerCount() == 0);

    // A move records nothing, and still changes what the layer holds.
    fixture.owner.inner.setBounds({20.f, 20.f, 10.f, 10.f});

    auto moved = fixture.render();

    check(fixture.host.getLastRenderedLayerCount() == 1, "a move");
    check(isColour(moved, 45, 45, green));
    check(isColour(moved, 35, 35, red));

    fixture.owner.inner.setVisible(false);

    auto hidden = fixture.render();

    check(fixture.host.getLastRenderedLayerCount() == 1, "a child hidden");
    check(isColour(hidden, 45, 45, red));
};

// A hidden child that was never painted goes on being owed a recording, and a
// layer judged by what is owed rather than by what was painted would be
// rendered every frame for as long as the child stays hidden.
auto tHiddenChildDoesNotKeepTheLayerBusy =
    test("Layer/aHiddenChildInsideTheCapturedSubtreeCostsNothing") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto fixture = CapturingHost {};
    auto popup = Fill {};

    popup.setVisible(false);
    fixture.owner.content.addChildComponent(popup);

    fixture.render();
    fixture.render();

    check(fixture.host.getLastRenderedLayerCount() == 0);
    check(fixture.host.getLastPaintedComponentCount() == 0);

    fixture.owner.inner.repaint();
    fixture.render();

    check(fixture.host.getLastPaintedComponentCount() == 1,
          "painted for the layer, and counted");

    fixture.owner.content.removeChildComponent(popup);
};

// Recorded by hand, as a headless snapshot does, before the frame that renders
// the layer: the recording is cleared without the layer having seen it, and
// the layer must still draw the new one.
auto tCapturedRecordedByHand =
    test("Layer/aCapturedSubtreeRecordedByHandIsStillRenderedAgain") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto fixture = CapturingHost {};
    fixture.owner.composite = true;

    fixture.render();

    fixture.owner.inner.colour = green;
    fixture.owner.inner.repaint();

    check(fixture.host.paintDirtyComponents() > 0);
    check(isColour(fixture.render(), 35, 35, green));
};

auto tClearingContentRestoresTheFrame =
    test("Layer/clearingTheContentPutsTheSubtreeBackInTheFrame") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto fixture = CapturingHost {};

    check(isEmpty(fixture.render(), 25, 25));

    fixture.owner.layer.setContent(nullptr);

    check(fixture.owner.layer.getContent() == nullptr);
    check(isColour(fixture.render(), 25, 25, red), "drawn by the frame again");
};

auto tDeletedContentIsLetGo = test("Layer/aLayerLetsGoOfContentThatIsDeleted") = []
{
    auto owner = Component {};
    auto layer = Layer {owner};
    auto content = std::make_unique<Component>();

    owner.addAndMakeVisible(*content);
    layer.setContent(content.get());

    check(layer.getContent() == content.get());

    content.reset();

    check(layer.getContent() == nullptr);
};

auto tRemovedContentIsLetGo = test("Layer/aLayerLetsGoOfContentThatIsRemoved") = []
{
    auto owner = Component {};
    auto content = Component {};
    auto layer = Layer {owner};

    owner.addAndMakeVisible(content);
    layer.setContent(&content);
    owner.removeChildComponent(content);

    check(layer.getContent() == nullptr);

    layer.setContent(&owner);

    check(layer.getContent() == nullptr, "and will not capture its own owner");
};

auto tDeletedContentRendersSafely =
    test("Layer/aFrameAfterTheContentIsDeletedDrawsNothingOfIt") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto host = ComponentHost {};
    auto owner = Component {};
    auto layer = Layer {owner};
    auto content = std::make_unique<Fill>();

    owner.addAndMakeVisible(*content);
    layer.setContent(content.get());
    layer.setBounds({0.f, 0.f, 50.f, 50.f});

    host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
    host.setBounds({0.f, 0.f, 100.f, 100.f});
    host.resized();
    host.setRootComponent(owner);
    content->setBounds({0.f, 0.f, 50.f, 50.f});

    check(isEmpty(host.renderToImage(1.f), 25, 25));

    content.reset();

    check(layer.getContent() == nullptr);
    check(isEmpty(host.renderToImage(1.f), 25, 25));
};

namespace
{
struct LayerQuadVertex
{
    float position[2];
};
} // namespace
} // namespace LayerTests

EACP_SHADER_VALUE(LayerTests::LayerQuadVertex, Float2)

namespace LayerTests
{
namespace
{
constexpr LayerQuadVertex fullLayerQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

struct DepthQuad final : GPU::ShaderProgram
{
    DepthQuad() { compile(); }

    void define() override
    {
        auto position = vertexInput(&LayerQuadVertex::position);

        setPosition(float4(position, depth, 1.f));
        setFragment(float4(color, 1.f));
    }

    GPU::Uniform<GPU::Float> depth;
    GPU::Uniform<GPU::Float3> color;

    EACP_SHADER(depth, color)
};

// Two quads covering the layer, the near one drawn first: only a depth test
// leaves it showing.
struct Scene final : Component
{
    Scene()
        : layer(*this)
    {
        layer.onRender = [this](GPU::RenderPass& pass, const Rect& area)
        {
            ++renders;
            lastArea = area;

            if (!prepared)
            {
                quads.setVertices(fullLayerQuad);
                quads.prepare(layer.getSampleCount(),
                              layer.hasDepth(),
                              GPU::PrimitiveTopology::Triangles,
                              GPU::BlendMode::None,
                              layer.getColorFormat());
                prepared = true;
            }

            quads.depth = 0.25f;
            quads.color = Array {1.f, 0.f, 0.f};
            pass.draw(quads);

            quads.depth = 0.75f;
            quads.color = Array {0.f, 1.f, 0.f};
            pass.draw(quads);
        };
    }

    void paint(UI::Graphics& g) override { g.drawLayer(layer); }

    Layer layer;
    DepthQuad quads;
    bool prepared = false;
    int renders = 0;
    Rect lastArea;
};

struct SceneHost
{
    SceneHost()
    {
        host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
        host.setBounds({0.f, 0.f, 100.f, 100.f});
        host.resized();
        host.setRootComponent(scene);
        scene.layer.setBounds({20.f, 20.f, 50.f, 30.f});
    }

    Scene scene;
    ComponentHost host;
};
} // namespace

auto tOwnPassIsCalledWithTheContentArea =
    test("Layer/anOnRenderLayerIsHandedItsAreaInDevicePixels") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto fixture = SceneHost {};

    check(fixture.scene.layer.getColorFormat() == GPU::PixelFormat::BGRA8Unorm);
    check(!fixture.scene.layer.hasDepth());
    check(fixture.scene.layer.getSampleCount() == 1);

    fixture.scene.layer.setDepth(true);

    fixture.host.renderToImage(2.f);

    check(fixture.scene.renders == 1);
    check(sameRect(fixture.scene.lastArea, {0.f, 0.f, 100.f, 60.f}));
    check(fixture.scene.layer.getTexture().hasDepth());

    fixture.host.renderToImage(2.f);

    check(fixture.scene.renders == 1, "and not again while nothing changed");

    fixture.scene.layer.setDirty();
    fixture.host.renderToImage(2.f);

    check(fixture.scene.renders == 2, "until asked");
};

auto tOwnPassHasDepthAndComposites =
    test("Layer/anOnRenderLayerDepthTestsAndIsComposited") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto fixture = SceneHost {};
    fixture.scene.layer.setDepth(true);

    auto image = fixture.host.renderToImage(1.f);

    check(isColour(image, 45, 35, red), "the nearer quad, inside the bounds");
    check(isEmpty(image, 10, 10), "and nothing outside them");
    check(isEmpty(image, 80, 35));
};

auto tOwnPassMultisamples =
    test("Layer/aMultisampledOnRenderLayerIsResolvedAndComposited") = []
{
    if (!GPU::Device::shared().isValid()
        || !GPU::Device::shared().supportsSampleCount(4))
        return;

    auto fixture = SceneHost {};
    fixture.scene.layer.setDepth(true);
    fixture.scene.layer.setSampleCount(4);

    auto image = fixture.host.renderToImage(1.f);

    check(fixture.scene.layer.getTexture().sampleCount() == 4);
    check(isColour(image, 45, 35, red));
};

namespace
{
// A captured child and an onRender layer after it, which is the arrangement
// that samples one layer from another: the second has to follow the first.
struct CapturedScene final : Component
{
    CapturedScene()
        : captured(*this)
        , projected(*this)
    {
        addAndMakeVisible(content);
        captured.setContent(&content);

        projected.onRender = [this](GPU::RenderPass&, const Rect&)
        {
            ++renders;
            sawCapturedReady = !captured.isEmpty();
        };
    }

    void resized() override
    {
        content.setBounds({0.f, 0.f, 40.f, 40.f});
        captured.setBounds({0.f, 0.f, 40.f, 40.f});
        projected.setBounds({0.f, 0.f, 40.f, 40.f});
    }

    Fill content;
    Layer captured;
    Layer projected;
    int renders = 0;
    bool sawCapturedReady = false;
};
} // namespace

auto tOwnPassFollowsAnEarlierLayer =
    test("Layer/anOnRenderLayerIsRenderedAgainWhenAnEarlierOneIs") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto host = ComponentHost {};
    auto scene = CapturedScene {};

    host.setBounds({0.f, 0.f, 100.f, 100.f});
    host.resized();
    host.setRootComponent(scene);

    host.renderToImage(1.f);

    check(scene.renders == 1);
    check(scene.sawCapturedReady, "after the layer it samples");

    host.renderToImage(1.f);

    check(scene.renders == 1);

    scene.content.colour = green;
    scene.content.repaint();
    host.renderToImage(1.f);

    check(host.getLastRenderedLayerCount() == 2, "both of them");
    check(scene.renders == 2);
};
} // namespace LayerTests
