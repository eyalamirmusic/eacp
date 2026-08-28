#include "Common.h"

#include <cmath>

// The blend equation written out: RenderPipelineDescriptor::blend and
// ::colorWriteMask.
//
// Checked by drawing, like the blend modes beside them in ShaderBlendTests and
// for the same reason: neither has a CPU-side observable, and a pipeline that
// built says nothing about what the hardware then computed. Each case clears to
// a known colour, draws one full-screen fragment of another known colour over
// it, and reads back what the two became.
//
// The arithmetic is chosen so that every expected value is exact rather than
// near: the destination is (0.5, 0.25, 0, 1) and the source is (0.5, 1, 0, 0.5),
// all representable in 8 bits, so a case that lands within a texel of its
// answer landed on it.
//
// **The destination's alpha has to come out of every case as 1, and that is the
// harness's constraint rather than the feature's.** GPUView's snapshot read-back
// converts the premultiplied attachment to the straight alpha Graphics::Image
// holds, which divides each colour channel by the alpha beside it - so a case
// that leaves the alpha at 0.5 has the very numbers it is checking doubled on
// the way out, and one that leaves it at 0 reads back as transparent black.
// Every hand-built state below therefore pins its alpha half to (Zero, One),
// which keeps the clear's opaque alpha and makes the read-back the identity.
// That is not a limitation of the tests: the alpha equation is checked by the
// preset cases, whose alpha reaches 1 on its own.
//
// **Every case that can have a mirror has one.** A blend test that only checks
// "the result is not the source" passes when nothing drew at all, which is the
// cull-mode lesson one stage over. So the destination-factor cases are paired
// with the source-factor case that would produce a different number from the
// same fragment, and the write-mask cases assert both what survived and what
// did not.
//
// Runs on both backends; self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullScreenTriangle[] = {
    {{-1.f, -1.f}}, {{3.f, -1.f}}, {{-1.f, 3.f}}};

// The background every case blends onto. Deliberately not grey: three different
// channel values mean a case that swapped two channels is visible, and an
// opaque alpha means the destination-alpha factors have a value worth reading.
constexpr auto destinationColor = Graphics::Color {0.5f, 0.25f, 0.f, 1.f};

// The fragment every case blends with. Its alpha is a half so that the
// alpha-weighted factors are distinguishable from the unweighted ones, which is
// the whole difference between Doom-3-era `blend add` and eacp's Additive.
constexpr auto sourceR = 0.5f;
constexpr auto sourceG = 1.f;
constexpr auto sourceB = 0.f;
constexpr auto sourceA = 0.5f;

struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(color);
    }

    Uniform<Float4> color;

    EACP_SHADER(color)
};

// One case: a clear, one draw through the pipeline under test, and the pixel
// that came out.
struct BlendCaseView final : GPUView
{
    explicit BlendCaseView(const RenderPipelineDescriptor& pipeline)
    {
        setSampleCount(1);

        shader.color = Array {sourceR, sourceG, sourceB, sourceA};
        shader.setVertices(fullScreenTriangle);

        auto descriptor = pipeline;
        descriptor.sampleCount = sampleCount();

        shader.prepare(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({destinationColor});
        pass.draw(shader);
    }

    FlatShader shader;
};

// Renders one case and hands back the middle pixel, or an invalid colour if
// there is no device or the pipeline did not build.
struct Outcome
{
    bool ran = false;
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
};

Outcome render(const RenderPipelineDescriptor& pipeline)
{
    if (!Device::shared().isValid())
        return {};

    auto view = BlendCaseView {pipeline};

    if (!view.shader.pipeline().isValid())
        return {};

    view.setBounds({0.f, 0.f, 16.f, 16.f});

    auto image = view.renderToImage(1.f);

    if (!image.isValid())
        return {};

    const auto pixel = image.at(8, 8);

    return {true, pixel.r, pixel.g, pixel.b};
}

// A texel either way at 8 bits per channel, which is what an exact answer
// looks like after a round trip through the attachment.
bool isNear(float value, float target, float tolerance = 0.01f)
{
    return std::abs(value - target) <= tolerance;
}

RenderPipelineDescriptor withBlend(const BlendState& blend)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.blend = blend;
    return descriptor;
}

// A blend whose colour half is the caller's and whose alpha half keeps the
// destination's - see the note at the top on why every case here needs that.
BlendState colorEquation(BlendFactor source,
                         BlendFactor destination,
                         BlendOperation operation = BlendOperation::Add)
{
    auto blend = BlendState {};

    blend.enabled = true;
    blend.sourceColor = source;
    blend.destinationColor = destination;
    blend.colorOperation = operation;

    blend.sourceAlpha = BlendFactor::Zero;
    blend.destinationAlpha = BlendFactor::One;
    blend.alphaOperation = BlendOperation::Add;

    return blend;
}

// The equation a material system asks for most often after alpha blending, and
// the one eacp's four named modes could not express: source times one plus
// destination times one, unweighted by alpha. Doom 3 spells it `blend add`.
BlendState unweightedAdd()
{
    return colorEquation(BlendFactor::One, BlendFactor::One);
}

// `blend filter` / `modulate`: the source scaled by whatever is behind it, and
// nothing of the destination kept beyond that. The factor is the *destination's
// colour*, which no shader can read, which is why this one has no workaround.
BlendState modulate()
{
    return colorEquation(BlendFactor::DestinationColor, BlendFactor::Zero);
}
} // namespace

// (ONE, ONE). Every channel is the plain sum, clamped at 1: red 0.5 + 0.5 = 1,
// green 0.25 + 1 = 1 (clamped), blue 0 + 0 = 0.
//
// Its mirror is the Additive preset below, which is (SRC_ALPHA, ONE) and
// therefore weights the same fragment by its half alpha - so the two cases put
// different numbers in the red channel from identical inputs, and neither can
// be passing because nothing drew.
auto tUnweightedAdd = test("BlendState/oneOneSumsWithoutWeightingByAlpha") = []
{
    auto out = render(withBlend(unweightedAdd()));

    if (!out.ran)
        return;

    check(isNear(out.r, 1.f));   // 0.5 + 0.5, at the clamp
    check(isNear(out.g, 1.f));   // 0.25 + 1, clamped
    check(isNear(out.b, 0.f));   // nothing in either
};

auto tAdditivePresetWeights = test("BlendState/additivePresetWeightsBySourceAlpha") = []
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.blendMode = BlendMode::Additive;

    auto out = render(descriptor);

    if (!out.ran)
        return;

    // 0.5 * 0.5 + 0.5 = 0.75, against the 1.0 the unweighted sum gave.
    check(isNear(out.r, 0.75f));
    check(isNear(out.g, 0.75f)); // 1 * 0.5 + 0.25
    check(isNear(out.b, 0.f));
};

// (DST_COLOR, ZERO). The destination multiplied by the source and nothing else,
// so the result is the product channel by channel.
//
// Mirrored by (SRC_COLOR, ZERO) below, which multiplies the *source* by itself
// instead - the same two operands the other way round, producing a different
// answer in every channel where they differ.
auto tModulate = test("BlendState/destinationColorModulates") = []
{
    auto out = render(withBlend(modulate()));

    if (!out.ran)
        return;

    check(isNear(out.r, 0.25f)); // 0.5 * 0.5
    check(isNear(out.g, 0.25f)); // 1   * 0.25
    check(isNear(out.b, 0.f));
};

auto tSourceColorSquares = test("BlendState/sourceColorIsNotDestinationColor") = []
{
    auto out = render(
        withBlend(colorEquation(BlendFactor::SourceColor, BlendFactor::Zero)));

    if (!out.ran)
        return;

    check(isNear(out.r, 0.25f)); // 0.5 * 0.5, which happens to agree
    check(isNear(out.g, 1.f));   // 1 * 1, where the two part company
    check(isNear(out.b, 0.f));
};

// The DST_ALPHA family, which is how a fog or blend-light pass weights itself
// by what the depth fill left in the alpha channel. The background is opaque,
// so this is the source at full strength added to nothing.
auto tDestinationAlpha = test("BlendState/destinationAlphaWeightsTheSource") = []
{
    auto out = render(
        withBlend(colorEquation(BlendFactor::DestinationAlpha, BlendFactor::Zero)));

    if (!out.ran)
        return;

    // Destination alpha is 1, so the source survives whole.
    check(isNear(out.r, sourceR));
    check(isNear(out.g, sourceG));
    check(isNear(out.b, sourceB));
};

// Its mirror: one minus that same alpha is zero, so nothing of the source
// reaches the attachment and the background stands. Without this, the case
// above would pass on a backend that ignored the factor and wrote the source
// through.
auto tOneMinusDestinationAlpha =
    test("BlendState/oneMinusDestinationAlphaRemovesTheSource") = []
{
    auto out = render(withBlend(
        colorEquation(BlendFactor::OneMinusDestinationAlpha, BlendFactor::One)));

    if (!out.ran)
        return;

    check(isNear(out.r, destinationColor.r));
    check(isNear(out.g, destinationColor.g));
    check(isNear(out.b, destinationColor.b));
};

// The operation, not the factors. Reverse subtract takes the source away from
// the destination, which is the direction the name promises and the one that is
// easy to get backwards.
auto tReverseSubtract = test("BlendState/reverseSubtractTakesSourceFromDestination") = []
{
    auto out = render(withBlend(colorEquation(
        BlendFactor::One, BlendFactor::One, BlendOperation::ReverseSubtract)));

    if (!out.ran)
        return;

    check(isNear(out.r, 0.f));   // 0.5 - 0.5
    check(isNear(out.g, 0.f));   // 0.25 - 1, clamped at zero
    check(isNear(out.b, 0.f));
};

// And its mirror, the other direction, which produces a different number in the
// green channel from the same pair.
auto tSubtract = test("BlendState/subtractTakesDestinationFromSource") = []
{
    auto out = render(withBlend(colorEquation(
        BlendFactor::One, BlendFactor::One, BlendOperation::Subtract)));

    if (!out.ran)
        return;

    check(isNear(out.r, 0.f));    // 0.5 - 0.5
    check(isNear(out.g, 0.75f));  // 1 - 0.25, where the two directions differ
    check(isNear(out.b, 0.f));
};

// Writing a preset out by hand has to mean what naming it meant, or
// blendStateFor is not the single statement of a preset it claims to be.
//
// The two are compared with each other rather than against a number, because
// this case cannot pin its alpha half - the point of it is that the *whole*
// preset round-trips, alpha equation included - and an unpinned alpha is
// divided out of the colour channels by the read-back. Comparing the two paths
// is unaffected by that, since both are divided identically.
//
// So the third check is what stops this being two no-ops agreeing: whatever the
// preset does, it is not what BlendMode::None does.
auto tPresetsRoundTrip = test("BlendState/writingOutAPresetMatchesTheMode") = []
{
    auto byMode = RenderPipelineDescriptor {};
    byMode.blendMode = BlendMode::AlphaBlend;

    auto byState = withBlend(blendStateFor(BlendMode::AlphaBlend));

    auto unblended = RenderPipelineDescriptor {};
    unblended.blendMode = BlendMode::None;

    auto a = render(byMode);
    auto b = render(byState);
    auto none = render(unblended);

    if (!a.ran || !b.ran || !none.ran)
        return;

    check(isNear(a.r, b.r));
    check(isNear(a.g, b.g));
    check(isNear(a.b, b.b));

    check(!isNear(a.g, none.g));
};

// `blend` wins over `blendMode` outright rather than being merged with it.
auto tBlendOverridesTheMode = test("BlendState/theEquationOverridesTheNamedMode") = []
{
    auto descriptor = withBlend(unweightedAdd());
    descriptor.blendMode = BlendMode::None;

    auto out = render(descriptor);

    if (!out.ran)
        return;

    // BlendMode::None would have overwritten with the source alone.
    check(isNear(out.r, 1.f));
    check(isNear(out.g, 1.f));
};

// The write mask, gap 12's other half. Green is masked off, so the destination's
// green survives untouched while red is overwritten by the source - which is the
// pair that makes this a mask rather than a pipeline that drew nothing.
auto tWriteMaskKeepsChannels = test("BlendState/colorWriteMaskSpares_a_channel") = []
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.colorWriteMask.green = false;

    // Masked for the harness's sake as well as its own: an unmasked alpha would
    // let the half-transparent source through and the read-back would then
    // divide the two colour channels this is about. That the mask also spares
    // the alpha is exactly what the green check below proves it does.
    descriptor.colorWriteMask.alpha = false;

    auto out = render(descriptor);

    if (!out.ran)
        return;

    check(isNear(out.r, sourceR));                // written
    check(isNear(out.g, destinationColor.g));     // spared
    check(isNear(out.b, sourceB));                // written
};

// The whole mask off: the draw runs, and the attachment is exactly what the
// clear left. This is the case the additive-at-zero-alpha workaround stood in
// for, and its mirror is every case above, all of which changed the pixel.
auto tWriteMaskNone = test("BlendState/colorWriteMaskNoneWritesNothing") = []
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.colorWriteMask = ColorWriteMask::none();

    auto out = render(descriptor);

    if (!out.ran)
        return;

    check(isNear(out.r, destinationColor.r));
    check(isNear(out.g, destinationColor.g));
    check(isNear(out.b, destinationColor.b));
};

// A mask and a blend together, because the two are independent and a backend
// that applied the mask before the blend - or dropped one when the other was
// set - would still pass both of the cases above on their own.
auto tMaskAndBlendTogether = test("BlendState/theMaskSurvivesA_blend") = []
{
    auto descriptor = withBlend(unweightedAdd());
    descriptor.colorWriteMask.red = false;

    auto out = render(descriptor);

    if (!out.ran)
        return;

    check(isNear(out.r, destinationColor.r)); // masked, so the sum never lands
    check(isNear(out.g, 1.f));                // blended, 0.25 + 1 clamped
};
