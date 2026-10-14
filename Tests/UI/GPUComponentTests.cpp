#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <memory>

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace GPUComponentTests
{
namespace
{
const auto blue = Color {0.f, 0.f, 1.f, 1.f};
const auto red = Color {1.f, 0.f, 0.f, 1.f};
const auto green = Color {0.f, 1.f, 0.f, 1.f};

bool hasDevice()
{
    return GPU::Device::shared().isValid();
}

bool isColour(const eacp::Graphics::Image& image, int x, int y, const Color& c)
{
    auto pixel = image.at(x, y);

    return pixel.a > 0.9f && std::abs(pixel.r - c.r) < 0.1f
           && std::abs(pixel.g - c.g) < 0.1f && std::abs(pixel.b - c.b) < 0.1f;
}

bool isEmpty(const eacp::Graphics::Image& image, int x, int y)
{
    return image.at(x, y).a < 0.1f;
}

eacp::Graphics::MouseEvent mouseAt(Point position)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;

    return event;
}

struct FrameReport
{
    Point pixelSize;
    Point logicalSize;
    float backingScale = 0.f;
    GPU::TextureFormat format = GPU::TextureFormat::RGBA16Float;
    bool redirected = false;
    bool valid = false;
};

struct ClearComponent : GPUComponent
{
    void render(GPU::Frame& frame) override
    {
        ++renders;
        report = {frame.pixelSize(),
                  frame.logicalSize(),
                  frame.backingScale(),
                  frame.drawableFormat(),
                  frame.isRedirected(),
                  frame.isValid()};

        frame.beginPass({colour});
    }

    void update(Threads::FrameTime time) override
    {
        ++updates;
        lastTime = time;
    }

    void mouseDrag(const MouseEvent& event) override { lastDrag = event; }

    bool mouseWheelMove(const MouseEvent& event) override
    {
        lastWheel = event;
        return true;
    }

    using GPUComponent::renderNow;

    Color colour = blue;
    FrameReport report;
    int renders = 0;
    int updates = 0;
    Threads::FrameTime lastTime;
    MouseEvent lastDrag;
    MouseEvent lastWheel;
};

struct RestorableHost final : ComponentHost
{
    using ComponentHost::deviceRestored;
};

struct Fixture
{
    Fixture()
    {
        host.setBackgroundColour({0.f, 0.f, 0.f, 0.f});
        host.setBounds({0.f, 0.f, 100.f, 100.f});
        host.resized();
        host.setAnimationClockEnabled(false);
        host.setRootComponent(root);
    }

    Component root;
    RestorableHost host;
};
} // namespace

auto tClearLandsInBounds =
    test("GPUComponent/aPlainBeginPassFillsTheComponentInTheHost") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({20.f, 30.f, 40.f, 20.f});

    auto image = fixture.host.renderToImage(1.f);

    check(gpu.renders == 1);
    check(gpu.getRenderCount() == 1);
    check(fixture.host.getLastRenderedGPUComponentCount() == 1);
    check(isColour(image, 40, 40, blue), "inside the component");
    check(isColour(image, 21, 31, blue), "to its corner");
    check(isEmpty(image, 10, 10), "and not outside it");
    check(isEmpty(image, 70, 40));
    check(isEmpty(image, 40, 55));

    fixture.host.renderToImage(1.f);
    check(gpu.renders == 1, "not rendered again while nothing changed");
    check(fixture.host.getLastRenderedGPUComponentCount() == 0);

    gpu.colour = red;
    gpu.repaint();
    auto repainted = fixture.host.renderToImage(1.f);

    check(gpu.renders == 2, "until repainted");
    check(isColour(repainted, 40, 40, red));
};

auto tFrameAnswersForTheComponent =
    test("GPUComponent/theFrameReportsTheComponentsSizeAndScale") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({20.f, 30.f, 40.f, 20.f});

    auto image = fixture.host.renderToImage(2.f);

    check(gpu.report.redirected);
    check(gpu.report.valid);
    check(gpu.report.pixelSize.x == 80.f && gpu.report.pixelSize.y == 40.f,
          "the component's pixels, not the host's 200x200");
    check(gpu.report.logicalSize.x == 40.f && gpu.report.logicalSize.y == 20.f);
    check(gpu.report.backingScale == 2.f);
    check(gpu.report.format == GPU::TextureFormat::BGRA8Unorm);
    check(isColour(image, 80, 80, blue), "and the host still composites it");
};

auto tCompositedWhereItSits =
    test("GPUComponent/aMoveCompositesElsewhereWithoutRenderingAgain") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto panel = Component {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(panel);
    panel.setBounds({10.f, 10.f, 60.f, 60.f});
    panel.addAndMakeVisible(gpu);
    gpu.setBounds({10.f, 10.f, 20.f, 20.f});

    auto before = fixture.host.renderToImage(1.f);

    check(isColour(before, 25, 25, blue), "at its parent's corner plus its own");
    check(isEmpty(before, 45, 45));

    panel.setBounds({30.f, 30.f, 60.f, 60.f});
    auto after = fixture.host.renderToImage(1.f);

    check(gpu.renders == 1, "a move is a composite, not a render");
    check(isColour(after, 45, 45, blue));
    check(isEmpty(after, 25, 25));
};

auto tClippedByParent = test("GPUComponent/isClippedByItsParent") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto parent = Component {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(parent);
    parent.setBounds({0.f, 0.f, 50.f, 50.f});
    parent.addAndMakeVisible(gpu);
    gpu.setBounds({25.f, 25.f, 50.f, 50.f});

    auto image = fixture.host.renderToImage(1.f);

    check(isColour(image, 37, 37, blue), "inside the parent");
    check(isEmpty(image, 60, 60), "cut at the parent's edge");
    check(isEmpty(image, 37, 60));
};

auto tOpaqueAndTransparent =
    test("GPUComponent/opaqueIgnoresAlphaAndTransparentHonoursIt") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    gpu.colour = {0.5f, 0.f, 0.f, 0.5f};
    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 50.f, 50.f});

    auto opaque = fixture.host.renderToImage(1.f);
    auto solid = opaque.at(25, 25);

    check(!gpu.isTransparent());
    check(solid.a > 0.9f, "a solid rectangle whatever the alpha says");
    check(std::abs(solid.r - 0.5f) < 0.1f, "with the colour as it stands");

    gpu.setTransparent(true);
    auto transparent = fixture.host.renderToImage(1.f);
    auto seeThrough = transparent.at(25, 25);

    check(gpu.isTransparent());
    check(seeThrough.a > 0.4f && seeThrough.a < 0.6f, "premultiplied, half there");
    check(gpu.renders == 1, "a change of how it composites, not of what it is");
};

auto tOpacityComposites =
    test("GPUComponent/opacityAndTransformCompositeWithoutRenderingAgain") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 50.f, 50.f});
    fixture.host.renderToImage(1.f);

    gpu.setOpacity(0.5f);

    auto image = fixture.host.renderToImage(1.f);
    auto pixel = image.at(25, 25);

    check(pixel.a > 0.4f && pixel.a < 0.6f);
    check(gpu.getOpacity() == 0.5f);

    gpu.setOpacity(1.f);
    gpu.setTransform(GPUWidgets::AffineTransform::translation(20.f, 20.f));

    auto moved = fixture.host.renderToImage(1.f);

    check(isColour(moved, 30, 30, blue), "placed by the matrix");
    check(isEmpty(moved, 10, 10));
    check(gpu.renders == 1, "neither renders again");
};

namespace
{
struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace GPUComponentTests

EACP_SHADER_VALUE(GPUComponentTests::QuadVertex, Float2)

namespace GPUComponentTests
{
namespace
{
constexpr QuadVertex fullQuad[] = {
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
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, depth, 1.f));
        setFragment(float4(color, 1.f));
    }

    GPU::Uniform<GPU::Float> depth;
    GPU::Uniform<GPU::Float3> color;

    EACP_SHADER(depth, color)
};

struct DepthScene final : GPUComponent
{
    DepthScene()
    {
        setDepth(true);
        setSampleCount(4);
    }

    void render(GPU::Frame& frame) override
    {
        if (!prepared)
        {
            quads.setVertices(fullQuad);
            quads.prepare(sampleCount(),
                          hasDepth(),
                          GPU::PrimitiveTopology::Triangles,
                          GPU::BlendMode::None,
                          GPU::pixelFormatFor(frame.drawableFormat()));
            prepared = true;
        }

        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 0.f}});

        quads.depth = 0.25f;
        quads.color = Array {1.f, 0.f, 0.f};
        pass.draw(quads);

        quads.depth = 0.75f;
        quads.color = Array {0.f, 1.f, 0.f};
        pass.draw(quads);
    }

    DepthQuad quads;
    bool prepared = false;
};
} // namespace

auto tDepthAndMultisample =
    test("GPUComponent/aDepthTestedMultisampledPassIsResolvedAndComposited") = []
{
    if (!hasDevice() || !GPU::Device::shared().supportsSampleCount(4))
        return;

    auto fixture = Fixture {};
    auto scene = DepthScene {};

    fixture.root.addAndMakeVisible(scene);
    scene.setBounds({20.f, 20.f, 50.f, 30.f});

    auto image = fixture.host.renderToImage(1.f);

    check(scene.getLayer().getTexture().sampleCount() == 4);
    check(scene.getLayer().getTexture().hasDepth());
    check(isColour(image, 45, 35, red), "the nearer quad wins");
    check(isEmpty(image, 10, 10));
    check(isEmpty(image, 80, 35));
};

auto tStencil = test("GPUComponent/aStencilPlaneComesWithItsDepthBuffer") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    gpu.setStencil(true);
    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 10.f, 10.f});
    fixture.host.renderToImage(1.f);

    check(gpu.getLayer().getTexture().hasStencil());
    check(gpu.getLayer().getTexture().hasDepth());

    gpu.setDepth(false);
    fixture.host.renderToImage(1.f);

    check(!gpu.hasStencil());
    check(!gpu.getLayer().getTexture().hasDepth(), "the texture is remade");
    check(gpu.renders == 2);
};

// FrameComputeTests' ComputeThenDrawView, base class switched from GPUView to
// GPUComponent and nothing else.
namespace
{
constexpr auto channelBase = 0.25f;
constexpr auto channelStep = 0.25f;
constexpr auto channelCount = 3;

struct InstanceColor
{
    float color[3];
};

struct ColorKernel final : GPU::ComputeProgram
{
    ColorKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, base + toFloat(i) * stepSize);
    }

    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::Float> base;
    GPU::Uniform<GPU::Float> stepSize;

    EACP_SHADER(output, base, stepSize)
};

struct ColorFromBuffer final : GPU::ShaderProgram
{
    ColorFromBuffer() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto color = instanceInput(&InstanceColor::color, 1);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(varying(color), 1.f));
    }

    EACP_SHADER()
};

struct ComputeThenDrawView final : GPUComponent
{
    ComputeThenDrawView()
        : colors(GPU::Device::shared(),
                 nullptr,
                 sizeof(float) * channelCount,
                 GPU::BufferUsage::Storage)
    {
        setSampleCount(1);

        kernel.output = colors;
        kernel.base = channelBase;
        kernel.stepSize = channelStep;
        kernel.prepare();

        draw.setVertices(fullQuad, 6);
        draw.setInstanceBuffer(1, colors, 1);
        draw.prepare(sampleCount());
    }

    void render(GPU::Frame& frame) override
    {
        {
            auto compute = frame.beginCompute();
            compute.dispatch(kernel, channelCount);
        }

        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.drawInstanced(draw, 1);
    }

    GPU::Buffer colors;
    ColorKernel kernel;
    ColorFromBuffer draw;
};

struct SampleTexture final : GPU::ShaderProgram
{
    SampleTexture() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = float2(position.x() * 0.5f + 0.5f, 0.5f - position.y() * 0.5f);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(sample(source, varying(uv)));
    }

    GPU::Uniform<GPU::Texture2D> source;

    EACP_SHADER(source)
};

// Draws whatever the component before it in the tree rendered.
struct Sampler final : GPUComponent
{
    explicit Sampler(const GPUComponent& sourceToUse)
        : source(sourceToUse)
    {
        program.setVertices(fullQuad, 6);
        program.prepare(sampleCount());
    }

    void render(GPU::Frame& frame) override
    {
        program.source = source.getLayer().getTexture();

        auto pass = frame.beginPass({red});
        pass.draw(program);
    }

    const GPUComponent& source;
    SampleTexture program;
};
} // namespace

auto tComputeThenDraw = test("GPUComponent/aComputePassFeedsTheDrawInOneRender") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto view = ComputeThenDrawView {};

    fixture.root.addAndMakeVisible(view);
    view.setBounds({10.f, 10.f, 20.f, 20.f});

    auto image = fixture.host.renderToImage(1.f);

    check(isColour(image, 20, 20, {0.25f, 0.5f, 0.75f, 1.f}),
          "the kernel's colour, drawn from the buffer it wrote");
    check(isEmpty(image, 5, 5));
};

auto tTreeOrder =
    test("GPUComponent/aLaterComponentSamplesAnEarlierOnesTexture") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto first = ClearComponent {};
    auto second = Sampler {first};

    fixture.root.addAndMakeVisible(first);
    fixture.root.addAndMakeVisible(second);
    first.setBounds({0.f, 0.f, 20.f, 20.f});
    second.setBounds({50.f, 50.f, 20.f, 20.f});
    first.colour = green;

    auto image = fixture.host.renderToImage(1.f);

    check(isColour(image, 10, 10, green));
    check(isColour(image, 60, 60, green), "drawn from the first one's texture");

    first.colour = blue;
    first.repaint();
    second.repaint();

    auto again = fixture.host.renderToImage(1.f);

    check(isColour(again, 60, 60, blue), "rendered first in the same frame");
};

auto tRepaintAndContinuous =
    test("GPUComponent/aRepaintRendersAndAContinuousTickPaintsNothing") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 20.f, 20.f});
    fixture.host.renderToImage(1.f);

    gpu.repaint();
    fixture.host.renderToImage(1.f);

    check(gpu.renders == 2);
    check(fixture.host.getLastPaintedComponentCount() == 1,
          "a repaint records the quad again");

    gpu.setContinuous(true);
    fixture.host.advanceAnimations(0.1);
    fixture.host.renderToImage(1.f);

    check(gpu.updates == 1);
    check(gpu.renders == 3, "a tick renders");
    check(fixture.host.getLastPaintedComponentCount() == 0, "and paints nothing");
};

auto tExactResize =
    test("GPUComponent/aResizeReallocatesExactlyEvenWhenShrinking") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 40.f, 20.f});
    fixture.host.renderToImage(1.f);

    check(gpu.getLayer().getTexture().width() == 40);
    check(gpu.getLayer().getTexture().height() == 20);

    gpu.setBounds({0.f, 0.f, 10.f, 5.f});
    auto image = fixture.host.renderToImage(1.f);

    check(gpu.renders == 2);
    check(gpu.getLayer().getTexture().width() == 10, "shrunk, not kept");
    check(gpu.getLayer().getTexture().height() == 5);
    check(gpu.report.pixelSize.x == 10.f && gpu.report.pixelSize.y == 5.f);
    check(isColour(image, 5, 2, blue));
    check(isEmpty(image, 20, 2));

    gpu.setBounds({0.f, 0.f, 0.f, 5.f});
    fixture.host.renderToImage(1.f);

    check(gpu.renders == 2, "nothing to render at zero size");
};

auto tCulled = test("GPUComponent/scrolledOutIsCulledAndRendersWhenBack") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto viewport = Component {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(viewport);
    viewport.setBounds({0.f, 0.f, 50.f, 50.f});
    viewport.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 60.f, 20.f, 20.f});

    fixture.host.renderToImage(1.f);

    check(gpu.renders == 0, "nothing of it inside the parent");
    check(gpu.wasCulledLastFrame());
    check(!gpu.isOnScreen());
    check(fixture.host.getLastCulledGPUComponentCount() == 1);
    check(fixture.host.getLastRenderedGPUComponentCount() == 0);

    gpu.setBounds({0.f, 10.f, 20.f, 20.f});
    auto image = fixture.host.renderToImage(1.f);

    check(gpu.renders == 1, "rendered the frame it is back");
    check(!gpu.wasCulledLastFrame());
    check(fixture.host.getLastCulledGPUComponentCount() == 0);
    check(isColour(image, 10, 20, blue));
};

auto tVisibleBounds = test("GPUComponent/reportsWhatOfItIsOnScreen") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto parent = Component {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(parent);
    parent.setBounds({0.f, 0.f, 50.f, 50.f});
    parent.addAndMakeVisible(gpu);
    gpu.setBounds({25.f, 30.f, 50.f, 50.f});

    fixture.host.renderToImage(1.f);

    auto visible = gpu.getVisibleBounds();

    check(gpu.isOnScreen());
    check(visible.x == 0.f && visible.y == 0.f);
    check(visible.w == 25.f && visible.h == 20.f, "cut by the parent");

    gpu.setBounds({60.f, 0.f, 50.f, 50.f});
    fixture.host.renderToImage(1.f);

    check(!gpu.isOnScreen());
    check(gpu.getVisibleBounds().isEmpty());

    gpu.setBounds({0.f, 0.f, 50.f, 50.f});
    fixture.host.renderToImage(1.f);
    check(gpu.isOnScreen());

    parent.setVisible(false);

    check(!gpu.isShowing());
    check(!gpu.isOnScreen(), "hidden is off screen at once");
    check(gpu.getVisibleBounds().isEmpty());
};

auto tHidden = test("GPUComponent/aHiddenOneIsNotRenderedAndKeepsItsTexture") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto page = Component {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(page);
    page.setBounds({0.f, 0.f, 50.f, 50.f});
    page.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 20.f, 20.f});
    fixture.host.renderToImage(1.f);

    page.setVisible(false);
    gpu.repaint();
    auto hidden = fixture.host.renderToImage(1.f);

    check(gpu.renders == 1, "a hidden page's component is not rendered");
    check(isEmpty(hidden, 10, 10));
    check(!gpu.getLayer().isEmpty(), "and keeps what it had");
    check(fixture.host.getLastCulledGPUComponentCount() == 0, "hidden, not culled");
    check(fixture.host.getLastHiddenGPUComponentCount() == 1);
    check(fixture.host.getLastRenderedGPUComponentCount() == 0);
    check(gpu.wasHiddenLastFrame());
    check(!gpu.wasCulledLastFrame());
    check(!gpu.isOnScreen());

    page.setVisible(true);
    auto shown = fixture.host.renderToImage(1.f);

    check(gpu.renders == 2, "the repaint it missed");
    check(isColour(shown, 10, 10, blue));
    check(!gpu.wasHiddenLastFrame());
    check(fixture.host.getLastHiddenGPUComponentCount() == 0);
};

auto tCountsAreDisjoint =
    test("GPUComponent/renderedCulledAndHiddenCountsAreDisjoint") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto viewport = Component {};
    auto page = Component {};
    auto shown = ClearComponent {};
    auto scrolledOut = ClearComponent {};
    auto zeroSize = ClearComponent {};
    auto hiddenItself = ClearComponent {};
    auto onHiddenPage = ClearComponent {};
    auto culledOnHiddenPage = ClearComponent {};

    fixture.root.addAndMakeVisible(viewport);
    viewport.setBounds({0.f, 0.f, 50.f, 50.f});
    viewport.addChildren({shown, scrolledOut, zeroSize, hiddenItself});
    shown.setBounds({0.f, 0.f, 10.f, 10.f});
    scrolledOut.setBounds({0.f, 70.f, 10.f, 10.f});
    zeroSize.setBounds({20.f, 0.f, 0.f, 10.f});
    hiddenItself.setBounds({30.f, 0.f, 10.f, 10.f});
    hiddenItself.setVisible(false);

    fixture.root.addAndMakeVisible(page);
    page.setBounds({0.f, 50.f, 50.f, 50.f});
    page.addChildren({onHiddenPage, culledOnHiddenPage});
    onHiddenPage.setBounds({0.f, 0.f, 10.f, 10.f});
    culledOnHiddenPage.setBounds({0.f, 90.f, 10.f, 10.f});
    page.setVisible(false);

    fixture.host.renderToImage(1.f);

    check(fixture.host.getLastRenderedGPUComponentCount() == 1);
    check(fixture.host.getLastCulledGPUComponentCount() == 2);
    check(fixture.host.getLastHiddenGPUComponentCount() == 3);

    check(!shown.wasCulledLastFrame() && !shown.wasHiddenLastFrame());
    check(shown.isOnScreen());

    for (auto* culled: {&scrolledOut, &zeroSize})
    {
        check(culled->wasCulledLastFrame());
        check(!culled->wasHiddenLastFrame());
        check(!culled->isOnScreen());
    }

    for (auto* hidden: {&hiddenItself, &onHiddenPage, &culledOnHiddenPage})
    {
        check(hidden->wasHiddenLastFrame());
        check(!hidden->wasCulledLastFrame(), "hidden is never culled too");
        check(!hidden->isOnScreen());
    }

    fixture.host.renderToImage(1.f);

    check(fixture.host.getLastRenderedGPUComponentCount() == 0,
          "on screen with nothing new is in none of them");
    check(fixture.host.getLastCulledGPUComponentCount() == 2);
    check(fixture.host.getLastHiddenGPUComponentCount() == 3);

    viewport.removeChildComponent(scrolledOut);

    check(scrolledOut.wasHiddenLastFrame(), "out of the host is hidden");
    check(!scrolledOut.wasCulledLastFrame());
};

namespace
{
struct TargetWatcher final : ClearComponent
{
    void targetChanged(GPU::Frame& frame) override
    {
        ++changes;
        changedRedirected = frame.isRedirected();
        rendersAtChange = renders;
        samplesAtChange = getLayer().getTexture().sampleCount();
    }

    int changes = 0;
    int rendersAtChange = -1;
    int samplesAtChange = 0;
    bool changedRedirected = false;
};
} // namespace

auto tTargetChanged =
    test("GPUComponent/targetChangedPrecedesRenderOnlyWhenTheTargetDoes") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = TargetWatcher {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 20.f, 20.f});

    fixture.host.renderToImage(1.f);

    check(gpu.changes == 1, "once, before the first render");
    check(gpu.rendersAtChange == 0, "and before render()");
    check(gpu.changedRedirected, "under the redirect");

    gpu.repaint();
    fixture.host.renderToImage(1.f);
    check(gpu.changes == 1, "not for a repaint");

    gpu.setBounds({0.f, 0.f, 30.f, 10.f});
    fixture.host.renderToImage(1.f);
    check(gpu.renders == 3);
    check(gpu.changes == 1, "nor for a resize");

    fixture.host.renderToImage(2.f);
    check(gpu.renders == 4);
    check(gpu.changes == 1, "nor for a new scale");

    gpu.setDepth(true);
    fixture.host.renderToImage(2.f);
    check(gpu.changes == 2, "a depth buffer is a new target");

    gpu.setStencil(true);
    fixture.host.renderToImage(2.f);
    check(gpu.changes == 3, "so is a stencil plane");

    if (GPU::Device::shared().supportsSampleCount(4))
    {
        gpu.setSampleCount(4);
        fixture.host.renderToImage(2.f);
        check(gpu.changes == 4, "and a sample count");
        check(gpu.samplesAtChange == 4, "with the new texture already made");
    }
};

auto tGpuTimingOff = test("GPUComponent/gpuTimingIsOffAndZeroByDefault") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 20.f, 20.f});

    for (auto frame = 0; frame < 4; ++frame)
    {
        gpu.repaint();
        fixture.host.renderToImage(1.f);
    }

    check(!gpu.isGpuTiming());
    check(gpu.lastGpuMilliseconds() == 0.0);

    for (const auto& pass: GPU::Device::shared().lastFrameTimings().passes)
        check(!pass.label.starts_with("GPUComponent#"), "nothing is labelled");
};

auto tGpuTimingOn = test("GPUComponent/gpuTimingSumsTheComponentsOwnPasses") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto timed = ClearComponent {};
    auto untimed = ClearComponent {};

    fixture.root.addChildren({timed, untimed});
    timed.setBounds({0.f, 0.f, 50.f, 50.f});
    untimed.setBounds({50.f, 0.f, 50.f, 50.f});
    timed.setGpuTiming(true);

    check(timed.isGpuTiming());
    check(timed.lastGpuMilliseconds() == 0.0, "nothing before it renders");

    for (auto frame = 0; frame < 4; ++frame)
    {
        timed.repaint();
        untimed.repaint();
        fixture.host.renderToImage(1.f);
        check(timed.lastGpuMilliseconds() >= 0.0);
    }

    check(untimed.lastGpuMilliseconds() == 0.0);

    if (!GPU::Device::shared().supportsPassTimings())
        return;

    auto own = 0;

    for (const auto& pass: GPU::Device::shared().lastFrameTimings().passes)
        if (pass.label.starts_with("GPUComponent#"))
            ++own;

    check(own == 1, "one pass timed, the timed component's");
    check(timed.lastGpuMilliseconds() > 0.0);

    timed.setGpuTiming(false);
    check(timed.lastGpuMilliseconds() == 0.0, "and zero again once off");
};

namespace
{
struct CapturedPanel final : Component
{
    CapturedPanel()
        : layer(*this)
    {
        addAndMakeVisible(content);
        layer.setContent(&content);
    }

    void resized() override
    {
        content.setBounds(getLocalBounds());
        layer.setBounds(getLocalBounds());
    }

    void paint(UI::Graphics& g) override { g.drawLayer(layer); }

    Component content;
    Layer layer;
};
} // namespace

auto tCaptured =
    test("GPUComponent/insideACapturedSubtreeItIsDrawnInTheCapture") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto panel = CapturedPanel {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(panel);
    panel.setBounds({10.f, 10.f, 60.f, 60.f});
    panel.content.addAndMakeVisible(gpu);
    gpu.setBounds({10.f, 10.f, 20.f, 20.f});

    auto image = fixture.host.renderToImage(1.f);

    check(gpu.renders == 1);
    check(!gpu.wasCulledLastFrame());
    check(isColour(image, 30, 30, blue), "through the panel's layer");

    panel.layer.setOpacity(0.5f);
    gpu.colour = red;
    gpu.repaint();

    auto faded = fixture.host.renderToImage(1.f);
    auto pixel = faded.at(30, 30);

    check(gpu.renders == 2);
    check(pixel.a > 0.4f && pixel.a < 0.6f, "faded as one group");
    check(pixel.r > 0.4f && pixel.b < 0.2f, "with the new picture");
};

auto tBackingScale = test("GPUComponent/aScaleChangeIsReportedAndRendersAgain") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};
    auto reported = 0.f;
    auto onScale = [&reported](float scale) { reported = scale; };

    gpu.onBackingScaleChanged = onScale;
    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 20.f, 20.f});

    fixture.host.renderToImage(1.f);
    check(reported == 0.f, "not for the first scale");

    fixture.host.renderToImage(2.f);

    check(reported == 2.f);
    check(gpu.renders == 2);
    check(gpu.report.pixelSize.x == 40.f);
};

auto tContinuousTicksUntilRemoved =
    test("GPUComponent/continuousModeUpdatesAndStopsWhenRemoved") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    gpu.setContinuous(true);
    check(!gpu.isAnimating(), "nothing to tick it outside a host");

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 20.f, 20.f});

    check(fixture.host.isAnimating(), "joining the host starts the clock");

    fixture.host.renderToImage(1.f);
    check(gpu.renders == 1);

    fixture.host.advanceAnimations(0.5);

    check(gpu.updates == 1);
    check(gpu.lastTime.delta == 0.5 && gpu.lastTime.time == 0.5);

    fixture.host.advanceAnimations(0.25);

    check(gpu.updates == 2);
    check(gpu.lastTime.time == 0.75);

    fixture.host.renderToImage(1.f);
    check(gpu.renders == 2, "an update asks for a render");

    fixture.root.removeChildComponent(gpu);

    check(!fixture.host.isAnimating());

    fixture.host.advanceAnimations(0.5);
    check(gpu.updates == 2, "and a removed one is not ticked");

    fixture.root.addAndMakeVisible(gpu);
    check(fixture.host.isAnimating(), "added back, it resumes");

    gpu.setContinuous(false);
    fixture.host.advanceAnimations(0.5);

    check(gpu.updates == 2, "nor one switched off");
    check(!fixture.host.isAnimating());
};

auto tReparent = test("GPUComponent/aReparentedContinuousComponentResumes") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto first = Component {};
    auto second = Component {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(first);
    fixture.root.addAndMakeVisible(second);
    first.setBounds({0.f, 0.f, 50.f, 50.f});
    second.setBounds({50.f, 50.f, 50.f, 50.f});

    first.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 20.f, 20.f});
    gpu.setContinuous(true);
    fixture.host.advanceAnimations(0.1);

    check(gpu.updates == 1);

    second.addAndMakeVisible(gpu);

    check(gpu.isAnimating());
    fixture.host.advanceAnimations(0.1);
    check(gpu.updates == 2);

    auto image = fixture.host.renderToImage(1.f);
    check(isColour(image, 60, 60, blue), "and draws where it went");

    auto detached = Component {};
    detached.addAndMakeVisible(gpu);
    check(!gpu.isAnimating(), "a subtree outside the host has no clock");

    fixture.root.addAndMakeVisible(detached);
    check(gpu.isAnimating(), "until the subtree joins it");
};

auto tCulledContinuous =
    test("GPUComponent/aCulledContinuousComponentUpdatesWithoutRendering") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto viewport = Component {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(viewport);
    viewport.setBounds({0.f, 0.f, 50.f, 50.f});
    viewport.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 80.f, 20.f, 20.f});
    gpu.setContinuous(true);

    fixture.host.renderToImage(1.f);
    check(gpu.wasCulledLastFrame());

    for (auto tick = 0; tick < 5; ++tick)
    {
        fixture.host.advanceAnimations(0.1);
        fixture.host.renderToImage(1.f);
    }

    check(gpu.updates == 5, "its state keeps moving off screen");
    check(gpu.renders == 0, "and nothing renders");
    check(!gpu.isOnScreen());
    check(fixture.host.isAnimating(), "the clock keeps running for it");

    viewport.setVisible(false);
    fixture.host.advanceAnimations(0.1);

    check(gpu.updates == 6, "hidden, too");

    fixture.host.renderToImage(1.f);

    check(gpu.wasHiddenLastFrame());
    check(!gpu.wasCulledLastFrame(), "hidden, and so no longer culled");
    check(fixture.host.getLastCulledGPUComponentCount() == 0);
    check(fixture.host.getLastHiddenGPUComponentCount() == 1);

    viewport.setVisible(true);
    gpu.setBounds({0.f, 10.f, 20.f, 20.f});
    fixture.host.renderToImage(1.f);

    check(gpu.renders == 1, "brought into view, it renders");
};

auto tMaxFps = test("GPUComponent/maxFpsPacesUpdates") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 20.f, 20.f});
    gpu.setMaxFps(60);
    gpu.setContinuous(true);

    check(gpu.maxFps() == 60);

    for (auto tick = 0; tick < 120; ++tick)
        fixture.host.advanceAnimations(1.0 / 120.0);

    check(gpu.updates == 60, "120 ticks at 120Hz, 60 updates");
    check(std::abs(gpu.lastTime.delta - 1.0 / 60.0) < 1e-9,
          "the delta spans the skipped tick");
    check(std::abs(gpu.lastTime.time - (119.0 / 120.0)) < 1e-9);

    fixture.host.setAnimationMaxFps(30);
    check(fixture.host.getAnimationMaxFps() == 30);
};

auto tDeletedWhileContinuous =
    test("GPUComponent/deletedWhileContinuousStopsCleanly") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = std::make_unique<ClearComponent>();

    fixture.root.addAndMakeVisible(*gpu);
    gpu->setBounds({0.f, 0.f, 20.f, 20.f});
    gpu->setContinuous(true);
    fixture.host.advanceAnimations(0.1);
    fixture.host.renderToImage(1.f);

    gpu.reset();

    check(!fixture.host.isAnimating());

    fixture.host.advanceAnimations(0.1);
    auto image = fixture.host.renderToImage(1.f);

    check(isEmpty(image, 10, 10));
};

auto tRenderNow = test("GPUComponent/renderNowRendersOrLeavesItToTheNextFrame") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 20.f, 20.f});
    fixture.host.renderToImage(1.f);

    gpu.renderNow();
    fixture.host.renderToImage(1.f);

    check(gpu.renders >= 2, "rendered now, or by the frame after");
    check(fixture.host.getLastPaintedComponentCount() == 0);
};

auto tDeviceRestored =
    test("GPUComponent/aRestoredDeviceRebuildsTheHostAndTellsTheComponent") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};
    auto restored = 0;
    auto onRestored = [&restored] { ++restored; };

    gpu.onDeviceRestored = onRestored;
    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({10.f, 10.f, 20.f, 20.f});
    fixture.host.renderToImage(1.f);

    fixture.host.deviceRestored();

    check(restored == 1);
    check(gpu.getLayer().isEmpty(), "the old texture is let go");

    auto image = fixture.host.renderToImage(1.f);

    check(gpu.renders == 2, "and rendered again into a new one");
    check(isColour(image, 20, 20, blue));
};

auto tDeviceRestoredChangesTheTarget =
    test("GPUComponent/aRestoredDeviceIsATargetChange") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = TargetWatcher {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({10.f, 10.f, 20.f, 20.f});
    fixture.host.renderToImage(1.f);

    check(gpu.changes == 1);

    fixture.host.deviceRestored();
    fixture.host.renderToImage(1.f);

    check(gpu.changes == 2, "pipelines from the old device are made again");
    check(gpu.rendersAtChange == 1, "before the render that follows");
};

auto tCopyDrawableCopiesTheComponent =
    test("GPUComponent/copyDrawableCopiesTheComponentsTexture") = []
{
    if (!hasDevice())
        return;

    struct Copier final : GPUComponent
    {
        void render(GPU::Frame& frame) override
        {
            auto descriptor = GPU::TextureDescriptor {};
            descriptor.width = (int) frame.pixelSize().x;
            descriptor.height = (int) frame.pixelSize().y;
            descriptor.format = frame.drawableFormat();

            copy.emplace(GPU::Device::shared(), descriptor, nullptr);

            frame.beginPass({blue});
            copied = frame.copyDrawable(*copy);
        }

        std::optional<GPU::Texture> copy;
        bool copied = false;
    };

    auto fixture = Fixture {};
    auto gpu = Copier {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({0.f, 0.f, 8.f, 4.f});

    fixture.host.renderToImage(1.f);

    check(gpu.copied);
    check(gpu.copy.has_value() && gpu.copy->width() == 8);

    unsigned char pixels[8 * 4 * 4] = {};
    gpu.copy->read(pixels);

    check(pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0 && pixels[3] == 255,
          "BGRA blue");
};

auto tInput = test("GPUComponent/rawDeltaPressureAndScrollPhaseReachIt") = []
{
    if (!hasDevice())
        return;

    auto fixture = Fixture {};
    auto gpu = ClearComponent {};

    fixture.root.addAndMakeVisible(gpu);
    gpu.setBounds({10.f, 10.f, 50.f, 50.f});

    fixture.host.mouseDown(mouseAt({20.f, 20.f}));

    auto drag = mouseAt({25.f, 22.f});
    drag.downPos = {20.f, 20.f};
    drag.delta = {5.f, 2.f};
    drag.rawDelta = {11.f, -7.f};
    drag.pressure = 0.25f;
    fixture.host.mouseDragged(drag);

    check(gpu.lastDrag.rawDelta.x == 11.f && gpu.lastDrag.rawDelta.y == -7.f);
    check(gpu.lastDrag.delta.x == 5.f);
    check(gpu.lastDrag.pressure == 0.25f);
    check(gpu.lastDrag.position.x == 15.f && gpu.lastDrag.position.y == 12.f);

    fixture.host.mouseUp(drag);

    auto wheel = mouseAt({30.f, 30.f});
    wheel.delta = {0.f, 3.f};
    wheel.scrollPhase = eacp::Graphics::ScrollPhase::Began;
    fixture.host.mouseWheel(wheel);

    check(gpu.lastWheel.scrollPhase == eacp::Graphics::ScrollPhase::Began);
    check(gpu.lastWheel.wheelDelta.y == 3.f);
};

auto tDefaults = test("GPUComponent/defaultsAndContract") = []
{
    auto gpu = ClearComponent {};

    check(gpu.sampleCount() == 1, "not a GPUView's 4");
    check(!gpu.hasDepth());
    check(!gpu.hasStencil());
    check(!gpu.isTransparent());
    check(!gpu.isContinuous());
    check(gpu.maxFps() == 0);
    check(gpu.getOpacity() == 1.f);
    check(gpu.getInterceptsMouseClicks());
    check(gpu.getRenderCount() == 0);
    check(gpu.colorFormat() == GPU::PixelFormat::BGRA8Unorm);
    check(gpu.backingScale() == 1.f, "outside a host");
    check(!gpu.isShowing());
    check(!gpu.isOnScreen());
    check(gpu.getVisibleBounds().isEmpty());
    check(!gpu.wasCulledLastFrame());

    gpu.setStencil(true);
    check(gpu.hasStencil() && gpu.hasDepth(), "stencil brings depth");

    gpu.setDepth(false);
    check(!gpu.hasStencil() && !gpu.hasDepth(), "and goes with it");

    gpu.setTransparent(true);
    check(gpu.isTransparent());

    gpu.setMaxFps(-5);
    check(gpu.maxFps() == 0);

    gpu.onBackingScaleChanged(2.f);
    gpu.onDeviceRestored();
};
} // namespace GPUComponentTests
