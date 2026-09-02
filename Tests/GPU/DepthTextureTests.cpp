#include "Common.h"

// TextureDescriptor::sampleableDepth and the shader slot it feeds:
// ShaderBuilder::depthTexture, bound with
// RenderPass::setFragmentDepthTexture. The pair is what lets a later pass read
// the depth an earlier one wrote - a soft particle fading where it meets the
// wall behind it, a fog whose thickness is how far away the geometry is.
//
// Three things are pinned.
//
// That a target says whether its depth buffer can be sampled, so a bind can be
// checked rather than found out at the draw. Asking for depth alone is *not*
// asking for this: on D3D12 the second question costs a typeless resource, a
// descriptor and a pair of barriers, and the control says the two are separate.
//
// That what comes back from the sample is the window-space depth the pass wrote
// - the clip-space z of the quad where it covered the target, and the far plane
// where nothing did. Both halves matter: the second is what says the value is
// the depth buffer's rather than something the copy invented.
//
// And that TextureFormat::R32Float keeps the value. That is not a separate
// subject - it is the format a depth copy has to land in, and eight bits or a
// half float would lose exactly the range a depth buffer spends its life in.
// 0.9994 is the number TheDarkMod's soft-particle shader clamps to and reads
// back as thirty thousand units; a half float has exactly one value between it
// and 1, and eight bits have none.
//
// **The depth is kept rather than discarded** (DepthAction::Keep), which is what
// makes it survive the end of the pass that wrote it. That is not a Windows-only
// concern: on an Apple M5 Max a pass that ends without Keep leaves a depth plane
// that samples as 0.0 everywhere, which Apps/GPU/DepthSampling found by dropping
// the flag to see. The load/store pairing itself is pinned in DepthActionTests;
// what this file pins is what a sample of a kept plane gives back.
//
// Runs on both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;
constexpr auto targetSize = 4;

// The depth the covering quad is drawn at, in clip space with w = 1 - so it
// reaches the depth buffer as it stands and this is the number the sample has to
// give back.
constexpr auto quadDepth = 0.25f;

// What a pass with a depth buffer clears it to, and so what the half of the
// target no quad covers has to read.
constexpr auto farPlane = 1.f;

// The value the R32Float check carries. Chosen for what it costs to store: it
// rounds to 255/255 in eight bits, and a half float lands on 0.99951, its last
// value below 1 - one step from the far plane. Only the full float keeps it as
// written.
constexpr auto nearlyFar = 0.9994f;

struct QuadVertex
{
    float position[2];
    float uv[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

// The left half only, so the right half of the depth buffer keeps its clear and
// the read can tell "the sample worked" from "everything reads the same".
constexpr QuadVertex leftHalf[] = {
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
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, quadDepth, 1.f));
        setFragment(float4(color, 1.f));
    }

    Uniform<Float3> color;

    EACP_SHADER(color)
};

// The depth of another target, straight out into a single-channel float target.
// One float in, one float out - which is the shape of a depth sample on both
// backends and the reason sample() gives back a Float here.
struct DepthCopyShader final : ShaderProgram
{
    DepthCopyShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));

        auto depth = sample(sceneDepth, varying(uv));

        setFragment(float4(depth, depth, depth, 1.f));
    }

    Uniform<TextureDepth2D> sceneDepth;

    EACP_SHADER(sceneDepth)
};

// A flat value into an R32Float target, for the precision half.
struct ValueShader final : ShaderProgram
{
    ValueShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(value, value, value, 1.f));
    }

    Uniform<Float> value;

    EACP_SHADER(value)
};

TextureDescriptor describeScene()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
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

constexpr auto pixelCount = targetSize * targetSize;

// Draws depth into one target and copies it out of that target's depth buffer
// into another, on one frame and in two passes - which is the only order this
// can happen in, a texture not being sampleable by the pass rendering into it.
struct DepthCopyView final : GPUView
{
    DepthCopyView()
        : scene(Device::shared().makeTexture(describeScene()))
        , copy(Device::shared().makeTexture(describeFloatTarget()))
    {
        setSampleCount(1);

        quad.setVertices(leftHalf, 6);
        quad.color = std::array {1.f, 0.f, 0.f};
        quad.prepare(1,
                     true,
                     PrimitiveTopology::Triangles,
                     BlendMode::None,
                     pixelFormatFor(TextureFormat::RGBA8Unorm));

        reader.setVertices(fullQuad, 6);
        reader.sceneDepth = scene;
        reader.prepare(1,
                       false,
                       PrimitiveTopology::Triangles,
                       BlendMode::None,
                       pixelFormatFor(TextureFormat::R32Float));
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
        copy.read(values.data());

        frame.beginPass({Graphics::Color::black()});
    }

    Texture scene;
    Texture copy;
    DepthQuadShader quad;
    DepthCopyShader reader;

    Array<float, pixelCount> values {};
};

// One flat value into an R32Float target and straight back out.
struct FloatTargetView final : GPUView
{
    FloatTargetView()
        : target(Device::shared().makeTexture(describeFloatTarget()))
    {
        setSampleCount(1);

        quad.setVertices(fullQuad, 6);
        quad.value = nearlyFar;
        quad.prepare(1,
                     false,
                     PrimitiveTopology::Triangles,
                     BlendMode::None,
                     pixelFormatFor(TextureFormat::R32Float));
    }

    void render(Frame& frame) override
    {
        {
            auto into = frame.beginPass(target, {Graphics::Color::black()});
            into.draw(quad);
        }

        frame.flush();
        target.read(values.data());

        frame.beginPass({Graphics::Color::black()});
    }

    Texture target;
    ValueShader quad;

    Array<float, pixelCount> values {};
};

// A frame has to be driven through the view, and the snapshot path is what
// drives one without a window.
template <typename View>
void renderOnce(View& view)
{
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    auto image = view.renderToImage(1.f);

    check(image.isValid());
}

float valueAt(const Array<float, pixelCount>& values, int x, int y)
{
    return values[(std::size_t) (y * targetSize + x)];
}

bool near(float value, float expected)
{
    return std::abs(value - expected) < 0.002f;
}
} // namespace

// A target says whether its depth can be sampled, and asking for depth is not
// asking for that. The two are separate because the second one costs D3D12 a
// typeless resource, a shader-visible descriptor and a barrier per pass.
auto tTargetReportsSampleableDepth =
    test("DepthTexture/aTargetSaysWhetherItsDepthCanBeSampled") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.renderTarget = true;

    auto plain = Device::shared().makeTexture(descriptor);

    check(!plain.hasDepth());
    check(!plain.hasSampleableDepth());

    descriptor.depth = true;
    auto attachOnly = Device::shared().makeTexture(descriptor);

    check(attachOnly.hasDepth());
    check(!attachOnly.hasSampleableDepth());

    descriptor.depth = false;
    descriptor.sampleableDepth = true;
    auto sampleable = Device::shared().makeTexture(descriptor);

    // Implies depth rather than standing beside it: there is nothing to sample
    // without a buffer to sample.
    check(sampleable.hasDepth());
    check(sampleable.hasSampleableDepth());
};

// Sampleable depth is still only meaningful for something that renders, so a
// plain texture asking for it gets no buffer at all - the rule
// TextureDescriptor::depth already has.
auto tSampleableDepthNeedsARenderTarget =
    test("DepthTexture/aPlainTextureGetsNoSampleableDepth") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.sampleableDepth = true;

    auto plain = Device::shared().makeTexture(descriptor);

    check(plain.isValid());
    check(!plain.hasDepth());
    check(!plain.hasSampleableDepth());
};

// The point of the pair: a second pass reads the depth the first one wrote, and
// what it reads is the clip-space z where the quad covered the target and the
// far plane where it did not.
auto tDepthReadsBackAsTheDepthThatWasWritten =
    test("DepthTexture/aLaterPassReadsTheDepthAnEarlierOneWrote") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = DepthCopyView {};

    if (!view.scene.hasSampleableDepth())
        return;

    renderOnce(view);

    // The left half is under the quad and the right half is not, which is what
    // makes this two readings rather than one.
    for (auto y = 0; y < targetSize; ++y)
    {
        check(near(valueAt(view.values, 0, y), quadDepth));
        check(near(valueAt(view.values, 1, y), quadDepth));
        check(near(valueAt(view.values, 2, y), farPlane));
        check(near(valueAt(view.values, 3, y), farPlane));
    }
};

// The format the copy lands in, and the reason it is not an eight-bit one: a
// depth buffer spends its range in the last thousandth, and this value is inside
// it.
auto tFloatTargetKeepsItsPrecision =
    test("DepthTexture/anR32FloatTargetKeepsTheValueItWasGiven") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = FloatTargetView {};

    if (!view.target.isRenderTarget())
        return;

    renderOnce(view);

    for (auto i = 0; i < pixelCount; ++i)
    {
        check(view.values[(std::size_t) i] == nearlyFar);
        check(view.values[(std::size_t) i] != 1.f);
    }
};
