#include "Common.h"

#include <cmath>

// TextureDescriptor::sampleCount: a render target that multisamples, drawn into
// through a multisampled texture beside it and resolved into the target at the
// end of every pass.
//
// Four things are pinned here, and the first two are the ones that say the
// feature is doing anything at all.
//
// That a target reports the count it was created with, and that a count the
// device refuses yields an *invalid* texture rather than one quietly dropping to
// one sample - the difference matters because every pipeline drawing there is
// compiled against the number, so a silent drop is a draw that is rejected much
// later, somewhere else.
//
// That the picture actually changes, and changes only where it should. The same
// diagonal triangle goes into a 4-sample target and a 1-sample one of the same
// size, and the two are compared pixel by pixel: the single-sampled one has no
// pixel that is neither black nor white, the multisampled one has a row of them
// along the edge, and every pixel away from the edge is identical in both. An
// assertion that merely said "the images differ" would pass for a target that
// had gone wrong in some other way.
//
// That a suspended pass across a multisampled target keeps **both** the colour
// and the stencil plane. This is the constraint the whole thing exists to
// satisfy: a texture cannot be sampled by the pass rendering into it, so an app
// copying the frame it is half way through composing ends its pass, copies, and
// opens another - and if the resolve had discarded the samples, the pass that
// resumed would load the flattened picture into a multisampled attachment and
// everything after the copy would be composited onto it. Both planes are checked
// because the two are stored by different clauses: the colour by the attachment
// keeping its samples, the depth-stencil by DepthAction.
//
// And that Texture::read after a Frame::flush comes back with the resolved
// picture, which is what a screenshot off a multisampled target is.
//
// Runs on both backends, and self-skips without a GPU device or without support
// for four samples.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto targetSize = 16;
constexpr auto samples = 4;

// What the depth-sampling case draws its quad at, in clip space with w = 1, so
// it reaches the depth buffer as it stands and this is the number a sample of
// the resolved plane has to give back.
constexpr auto quadDepth = 0.25f;

struct QuadVertex
{
    float position[2];
};

// The same thing with texture coordinates, for the pass that reads the depth
// buffer of the pass before it.
struct UvVertex
{
    float position[2];
    float uv[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)
EACP_SHADER_VALUE(UvVertex, Float2)

namespace
{
// A right triangle whose hypotenuse runs corner to corner, offset by a quarter
// of the frame so the edge misses every pixel centre: a diagonal passing exactly
// through them makes the single-sampled control depend on a rasterizer's
// tie-breaking rule rather than on coverage.
constexpr QuadVertex diagonal[] = {
    {{-1.f, -1.f}},
    {{0.75f, -1.f}},
    {{-1.f, 0.75f}},
};

// Counter-clockwise in clip space - the space setPosition writes, with y up.
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

constexpr QuadVertex leftHalf[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},
};

struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.5f, 1.f));
        setFragment(color);
    }

    Uniform<Float4> color;

    EACP_SHADER(color)
};

constexpr UvVertex fullUvQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

// The left half only, so the right half of the depth buffer keeps its clear and
// a read can tell "the sample worked" from "everything reads the same".
constexpr UvVertex leftUvHalf[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{0.f, -1.f}, {0.5f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{0.f, -1.f}, {0.5f, 1.f}},
    {{0.f, 1.f}, {0.5f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

// Writes depth and nothing worth looking at in colour.
struct DepthQuadShader final : ShaderProgram
{
    DepthQuadShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&UvVertex::position);

        setPosition(float4(position, quadDepth, 1.f));
        setFragment(float4(color, 1.f));
    }

    Uniform<Float3> color;

    EACP_SHADER(color)
};

// The depth of another target, straight out into a single-channel float target -
// the shape of DepthTextureTests' copy, and the shape of Doom 3's _currentDepth.
struct DepthCopyShader final : ShaderProgram
{
    DepthCopyShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&UvVertex::position);
        auto uv = vertexInput(&UvVertex::uv);

        setPosition(float4(position, 0.f, 1.f));

        auto depth = sample(sceneDepth, varying(uv));

        setFragment(float4(depth, depth, depth, 1.f));
    }

    Uniform<TextureDepth2D> sceneDepth;

    EACP_SHADER(sceneDepth)
};

TextureDescriptor describeTarget(int sampleCount, bool withStencil = false)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    descriptor.sampleCount = sampleCount;
    descriptor.stencil = withStencil;
    return descriptor;
}

TextureDescriptor describeSampleableTarget(int sampleCount)
{
    auto descriptor = describeTarget(sampleCount);
    descriptor.sampleableDepth = true;
    return descriptor;
}

TextureDescriptor describeFloatTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::R32Float;
    descriptor.renderTarget = true;
    return descriptor;
}

constexpr auto pixelBytes = targetSize * targetSize * 4;

using Readback = Array<unsigned char, pixelBytes>;

const unsigned char* pixelAt(const Readback& pixels, int x, int y)
{
    return pixels.data() + ((std::size_t) y * targetSize + (std::size_t) x) * 4;
}

// One triangle into a target of the given sample count, read back inside the
// frame that drew it. The read is the point of the flush, exactly as in
// TextureReadTests - and here it is also what proves the resolve happened,
// since the texture being read is the resolve destination and nothing else ever
// writes it.
struct EdgeView final : GPUView
{
    explicit EdgeView(int sampleCount)
        : target(Device::shared().makeTexture(describeTarget(sampleCount)))
    {
        setSampleCount(1);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.sampleCount = sampleCount;
        descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);

        triangle.color = Array {1.f, 1.f, 1.f, 1.f};
        triangle.setVertices(diagonal);
        triangle.prepare(descriptor);
    }

    void render(Frame& frame) override
    {
        {
            auto into = frame.beginPass(target, {Graphics::Color::black()});
            into.draw(triangle);
        }

        frame.flush();
        target.read(pixels.data());

        frame.beginPass({Graphics::Color::black()});
    }

    Texture target;
    FlatShader triangle;
    Readback pixels {};
};

// A pass that stamps a stencil value and a colour, a boundary, and a pass that
// resumes both: the second draw is stencil-tested, so where it survives says the
// plane crossed the boundary, and where it does not the colour underneath says
// the samples did.
struct SuspendedView final : GPUView
{
    SuspendedView()
        : target(Device::shared().makeTexture(describeTarget(samples, true)))
    {
        setSampleCount(1);

        auto basePipeline = RenderPipelineDescriptor {};
        basePipeline.sampleCount = samples;
        basePipeline.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
        basePipeline.stencil = true;

        // Blue over the whole frame, and no stencil of its own: what stamps the
        // plane is the left-half draw below, so that the two halves differ in
        // the plane and in nothing else.
        background.color = Array {0.f, 0.f, 1.f, 1.f};
        background.setVertices(fullQuad);
        background.prepare(basePipeline);

        auto writerPipeline = basePipeline;
        writerPipeline.stencilFront.pass = StencilOp::Replace;
        writerPipeline.stencilBack.pass = StencilOp::Replace;

        writer.color = Array {0.f, 0.f, 1.f, 1.f};
        writer.setVertices(leftHalf);
        writer.prepare(writerPipeline);

        auto testerPipeline = basePipeline;
        testerPipeline.stencilFront.compare = CompareFunction::Equal;
        testerPipeline.stencilBack.compare = CompareFunction::Equal;

        tester.color = Array {0.f, 1.f, 0.f, 1.f};
        tester.setVertices(fullQuad);
        tester.prepare(testerPipeline);
    }

    void render(Frame& frame) override
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = Graphics::Color::black();
            descriptor.depthAction = DepthAction::Keep;

            auto pass = frame.beginPass(target, descriptor);
            pass.setStencilReference(1);
            pass.draw(background);
            pass.draw(writer);
        }

        // The copy the suspension is for: a read of the target between the two
        // passes, which is only correct if the pass that ended resolved into it.
        frame.flush();
        target.read(afterSuspend.data());

        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clear = false;
            descriptor.depthAction = DepthAction::Resume;

            auto pass = frame.beginPass(target, descriptor);
            pass.setStencilReference(1);
            pass.draw(tester);
        }

        frame.flush();
        target.read(atEnd.data());

        frame.beginPass({Graphics::Color::black()});
    }

    Texture target;
    FlatShader background;
    FlatShader writer;
    FlatShader tester;

    Readback afterSuspend {};
    Readback atEnd {};
};

// A multisampled scene whose depth a second pass reads. A multisampled depth
// buffer is not something the generated shaders can declare - they say depth2d,
// not a depth2d_ms with a sample index - so the plane is resolved into a
// single-sampled twin and that is what the bind hands over. This is what says
// the twin exists, is filled in, and holds the depth the pass wrote.
struct DepthSampleView final : GPUView
{
    explicit DepthSampleView(int sampleCount)
        : scene(Device::shared().makeTexture(describeSampleableTarget(sampleCount)))
        , copy(Device::shared().makeTexture(describeFloatTarget()))
    {
        setSampleCount(1);

        auto scenePipeline = RenderPipelineDescriptor {};
        scenePipeline.sampleCount = sampleCount;
        scenePipeline.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
        scenePipeline.depth = true;

        quad.color = Array {1.f, 0.f, 0.f};
        quad.setVertices(leftUvHalf);
        quad.prepare(scenePipeline);

        auto copyPipeline = RenderPipelineDescriptor {};
        copyPipeline.sampleCount = 1;
        copyPipeline.colorFormat = pixelFormatFor(TextureFormat::R32Float);

        reader.setVertices(fullUvQuad);
        reader.sceneDepth = scene;
        reader.prepare(copyPipeline);
    }

    void render(Frame& frame) override
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = Graphics::Color::black();
            descriptor.depthAction = DepthAction::Keep;

            auto into = frame.beginPass(scene, descriptor);
            into.draw(quad);
        }

        {
            // Cleared to something no depth value could be, so a copy that drew
            // nothing at all cannot be mistaken for one that read the far plane.
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = {-1.f, 0.f, 0.f, 1.f};

            auto into = frame.beginPass(copy, descriptor);
            into.draw(reader);
        }

        frame.flush();
        copy.read(depths.data());

        frame.beginPass({Graphics::Color::black()});
    }

    Texture scene;
    Texture copy;
    DepthQuadShader quad;
    DepthCopyShader reader;

    Array<float, targetSize * targetSize> depths {};
};

// How many pixels are neither of the two colours the triangle can produce -
// which is what a resolve of partly covered samples makes and a single-sampled
// rasterizer cannot.
int blendedPixels(const Readback& pixels)
{
    auto count = 0;

    for (auto y = 0; y < targetSize; ++y)
    {
        for (auto x = 0; x < targetSize; ++x)
        {
            const auto value = pixelAt(pixels, x, y)[0];

            if (value > 16 && value < 239)
                ++count;
        }
    }

    return count;
}

bool supportsMultisampling()
{
    return Device::shared().isValid()
           && Device::shared().supportsSampleCount(samples);
}
} // namespace

// A target says what it takes, so a pipeline can be built to match rather than
// finding out at the draw - the same thing hasDepth answers one field over.
auto tTargetReportsItsSampleCount =
    test("MultisampledTarget/aTargetSaysHowManySamplesItTakes") = []
{
    if (!supportsMultisampling())
        return;

    auto single = Device::shared().makeTexture(describeTarget(1));

    check(single.isRenderTarget());
    check(single.sampleCount() == 1);

    auto multi = Device::shared().makeTexture(describeTarget(samples));

    check(multi.isRenderTarget());
    check(multi.sampleCount() == samples);
};

// Multisampling is a property of a pass, so asking for it on a texture nothing
// renders into is ignored rather than allocating a second texture nothing can
// attach - exactly what depth does.
auto tSampleCountNeedsARenderTarget =
    test("MultisampledTarget/aPlainTextureIsNeverMultisampled") = []
{
    if (!supportsMultisampling())
        return;

    auto descriptor = describeTarget(samples);
    descriptor.renderTarget = false;

    auto plain = Device::shared().makeTexture(descriptor);

    check(plain.isValid());
    check(plain.sampleCount() == 1);
};

// Refused rather than clamped. A count silently dropped to one would leave every
// pipeline compiled against the number the app asked for, and the draw would be
// rejected somewhere else entirely.
auto tRefusedCountYieldsNoTexture =
    test("MultisampledTarget/anUnsupportedCountIsNotATexture") = []
{
    if (!Device::shared().isValid())
        return;

    // Not a power of two, so no device offers it, and neither backend has a
    // reason to round it to one that is.
    constexpr auto impossible = 3;

    check(!Device::shared().supportsSampleCount(impossible));

    auto refused = Device::shared().makeTexture(describeTarget(impossible));

    check(!refused.isValid());

    // And the trivial count is true everywhere, so a caller that clamps has
    // somewhere to clamp to.
    check(Device::shared().supportsSampleCount(1));
};

// The case this exists for: the edge of a triangle comes back with intermediate
// values, which is what resolving partly covered samples produces.
auto tEdgesAreSmoothed = test("MultisampledTarget/theEdgeOfATriangleIsBlended") = []
{
    if (!supportsMultisampling())
        return;

    auto view = EdgeView {samples};

    check(view.target.sampleCount() == samples);

    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    view.renderToImage(1.f);

    // The diagonal crosses fifteen of the sixteen rows; a handful is enough to
    // say the resolve ran without pinning a particular sample pattern.
    check(blendedPixels(view.pixels) >= 4);
};

// The mirror, and the reason the case above is evidence rather than a target
// that had gone wrong some other way: the identical draw at one sample has no
// blended pixel at all, and the two agree everywhere the edge is not.
auto tInteriorsAreUnchanged = test("MultisampledTarget/onlyTheEdgeMoves") = []
{
    if (!supportsMultisampling())
        return;

    auto multi = EdgeView {samples};
    auto single = EdgeView {1};

    check(single.target.sampleCount() == 1);

    const auto bounds =
        Graphics::Rect {0.f, 0.f, (float) targetSize, (float) targetSize};

    multi.setBounds(bounds);
    multi.renderToImage(1.f);

    single.setBounds(bounds);
    single.renderToImage(1.f);

    check(blendedPixels(single.pixels) == 0);

    // Two pixels the edge is nowhere near, one deep inside the triangle and one
    // well outside it. Both are the same in the two images, which is what says
    // the multisampled one is the same picture with a softer edge rather than a
    // different picture.
    check(pixelAt(multi.pixels, 0, targetSize - 1)[0] > 239);
    check(pixelAt(single.pixels, 0, targetSize - 1)[0] > 239);

    check(pixelAt(multi.pixels, targetSize - 1, 0)[0] < 16);
    check(pixelAt(single.pixels, targetSize - 1, 0)[0] < 16);
};

// The constraint the port needed: a pass suspended over a multisampled target
// comes back with the colour it left *and* the stencil plane it wrote.
auto tSuspensionKeepsColourAndStencil =
    test("MultisampledTarget/aResumedPassKeepsColourAndStencil") = []
{
    if (!supportsMultisampling())
        return;

    auto view = SuspendedView {};

    check(view.target.sampleCount() == samples);
    check(view.target.hasStencil());

    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    view.renderToImage(1.f);

    // The read taken between the two passes sees the first pass's blue, which is
    // the resolve having run at the end of a pass rather than at the end of the
    // frame.
    const auto* midway = pixelAt(view.afterSuspend, targetSize / 4, targetSize / 2);

    check(midway[2] > 200);
    check(midway[1] < 55);

    // The left half was stencilled, so the second pass's green survives there.
    const auto* stencilled = pixelAt(view.atEnd, targetSize / 4, targetSize / 2);

    check(stencilled[1] > 200);
    check(stencilled[2] < 55);

    // The right half was not, so what stands is the colour the *first* pass drew
    // - loaded back out of the multisampled attachment the resolve did not
    // discard. A resolve-and-forget would leave this pixel black, the load
    // having come from a texture nothing wrote.
    const auto* kept = pixelAt(view.atEnd, 3 * targetSize / 4, targetSize / 2);

    check(kept[2] > 200);
    check(kept[1] < 55);
};

// The other constraint the port needed: sampleableDepth on a multisampled
// target. The plane a shader reads is the resolve rather than the attachment, so
// what this checks is that the resolve happened and carries the depth the pass
// wrote - the left half at the quad's own z, the right half at the far plane the
// pass cleared to.
auto tDepthResolvesForSampling =
    test("MultisampledTarget/theDepthPlaneResolvesForSampling") = []
{
    if (!supportsMultisampling())
        return;

    auto view = DepthSampleView {samples};

    check(view.scene.sampleCount() == samples);
    check(view.scene.hasSampleableDepth());

    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    view.renderToImage(1.f);

    const auto covered = view.depths[(targetSize / 2) * targetSize + 1];
    const auto empty = view.depths[(targetSize / 2) * targetSize + targetSize - 2];

    check(std::abs(covered - quadDepth) < 0.01f);
    check(std::abs(empty - 1.f) < 0.01f);
};

// The control that says the number above is the multisampled path's rather than
// something that would have come out either way: the identical scene at one
// sample reads the same two values through the attachment itself.
auto tSingleSampledDepthReadsTheSame =
    test("MultisampledTarget/oneSampleReadsTheSameDepth") = []
{
    if (!supportsMultisampling())
        return;

    auto view = DepthSampleView {1};

    check(view.scene.sampleCount() == 1);
    check(view.scene.hasSampleableDepth());

    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    view.renderToImage(1.f);

    const auto covered = view.depths[(targetSize / 2) * targetSize + 1];
    const auto empty = view.depths[(targetSize / 2) * targetSize + targetSize - 2];

    check(std::abs(covered - quadDepth) < 0.01f);
    check(std::abs(empty - 1.f) < 0.01f);
};
