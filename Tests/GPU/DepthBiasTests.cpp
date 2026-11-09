#include "Common.h"

#include <array>

// RenderPipelineDescriptor::depthBias, checked by drawing a reference quad and
// then a probe over it whose pipeline carries the bias, and reading back which
// of the two the depth test kept.
//
// The probe is coplanar with the reference unless a case says otherwise, so
// with no bias it passes LessEqual on equality and comes back green; every case
// is the bias deciding against that. Same program, same vertices, so nothing
// but the pipeline separates the two depths.
//
// Metal is the backend this most needs: bias is encoder state there, set by
// setPipeline, and an encoder keeps what the last pipeline set. D3D12 and Vulkan
// bake it into the pipeline object and cannot leak it.
//
// Runs on every backend, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace DepthBiasTests
{
namespace
{
constexpr auto viewWidth = 8;
constexpr auto viewHeight = 4;

struct QuadVertex
{
    float position[3];
};
} // namespace
} // namespace DepthBiasTests

EACP_SHADER_VALUE(DepthBiasTests::QuadVertex, Float3)

namespace DepthBiasTests
{
namespace
{
using Quad = std::array<QuadVertex, 6>;

// Counter-clockwise in clip space, z running linearly from left to right.
constexpr Quad quadFrom(float leftDepth, float rightDepth)
{
    return {{{{-1.f, -1.f, leftDepth}},
             {{1.f, -1.f, rightDepth}},
             {{-1.f, 1.f, leftDepth}},
             {{1.f, -1.f, rightDepth}},
             {{1.f, 1.f, rightDepth}},
             {{-1.f, 1.f, leftDepth}}}};
}

constexpr auto flatQuad = quadFrom(0.5f, 0.5f);

// 0.5 at the centre pixel like the flat one, but half the depth range across
// eight pixels, so its slope is about 0.06 a pixel.
constexpr auto tiltedQuad = quadFrom(0.25f, 0.75f);

// One unit of constant bias is 2^-24 anywhere in [0.5, 1).
constexpr auto unitAtThreeQuarters = 1.f / 16777216.f;
constexpr auto calibrationDepth = 0.75f;
constexpr auto calibrationUnits = 1000.f;

struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 1.f));
        setFragment(color);
    }

    Uniform<Float4> color;

    EACP_SHADER(color)
};

struct Scene
{
    DepthCompare compare = DepthCompare::LessEqual;
    float clearDepth = 1.f;

    Quad reference = flatQuad;
    Quad probe = flatQuad;
    DepthBias probeBias;

    // A biased pipeline bound between the two, which a pipeline without bias
    // must not inherit.
    bool lendBias = false;
};

struct BiasView final : GPUView
{
    explicit BiasView(const Scene& sceneToUse)
        : scene(sceneToUse)
    {
        setSampleCount(1);
        setDepth(true);

        reference.color = Array {1.f, 0.f, 0.f, 1.f};
        reference.setVertices(scene.reference.data(), 6);
        reference.prepare(describe({}));

        lender.color = Array {0.f, 0.f, 1.f, 1.f};
        lender.setVertices(scene.reference.data(), 6);
        lender.prepare(describe({1.0e6f, 4.f, 0.f}));

        probe.color = Array {0.f, 1.f, 0.f, 1.f};
        probe.setVertices(scene.probe.data(), 6);
        probe.prepare(describe(scene.probeBias));
    }

    RenderPipelineDescriptor describe(const DepthBias& bias) const
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.sampleCount = 1;
        descriptor.depth = true;
        descriptor.depthCompare = scene.compare;
        descriptor.depthBias = bias;
        return descriptor;
    }

    void render(Frame& frame) override
    {
        auto descriptor = RenderPassDescriptor {};
        descriptor.clearColor = Graphics::Color::black();
        descriptor.clearDepth = scene.clearDepth;

        auto pass = frame.beginPass(descriptor);
        pass.draw(reference);

        if (scene.lendBias)
            pass.draw(lender);

        pass.draw(probe);
    }

    Scene scene;
    FlatShader reference;
    FlatShader lender;
    FlatShader probe;
};

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f && c.b < 0.5f;
}

Graphics::Color centreOf(const Scene& scene)
{
    auto view = BiasView {scene};
    check(view.hasDepth());

    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    auto image = view.renderToImage(1.f);
    check(image.isValid());

    return image.at(viewWidth / 2, viewHeight / 2);
}

bool probeWins(const Scene& scene)
{
    const auto colour = centreOf(scene);
    check(isGreen(colour) || isRed(colour));
    return isGreen(colour);
}

Scene withConstant(float constant)
{
    auto scene = Scene {};
    scene.probeBias.constant = constant;
    return scene;
}

Scene reversed(Scene scene)
{
    scene.compare = DepthCompare::GreaterEqual;
    scene.clearDepth = 0.f;
    return scene;
}

// A probe calibrationUnits units of 2^-24 nearer than the reference, biased
// back by `units`: it wins while the bias has not carried it past.
Scene calibratedBy(float units)
{
    const auto nearer = calibrationDepth - calibrationUnits * unitAtThreeQuarters;

    auto scene = withConstant(units);
    scene.reference = quadFrom(calibrationDepth, calibrationDepth);
    scene.probe = quadFrom(nearer, nearer);
    return scene;
}
} // namespace

// The baseline every other case is measured against: unbiased, the coplanar
// probe passes LessEqual on equality.
auto tUnbiasedCoplanarPasses = test("DepthBias/anUnbiasedCoplanarDrawPasses") = []
{
    if (!Device::shared().isValid())
        return;

    check(probeWins({}));
};

// Positive bias makes depth larger: behind the reference under LessEqual, and
// so rejected; negative brings it nearer and it wins.
auto tPositiveIsDeeper = test("DepthBias/positiveBiasPushesDepthLarger") = []
{
    if (!Device::shared().isValid())
        return;

    check(!probeWins(withConstant(1024.f)));
    check(probeWins(withConstant(-1024.f)));
};

// The same bias under reversed Z, cleared to 0 and tested GreaterEqual: larger
// depth is nearer there, so the two signs swap outcomes.
auto tReversedSwapsTheSigns = test("DepthBias/reversedZSwapsWhichSignWins") = []
{
    if (!Device::shared().isValid())
        return;

    check(probeWins(reversed(withConstant(1024.f))));
    check(!probeWins(reversed(withConstant(-1024.f))));
};

// The unit is the depth plane's precision at the primitive's depth, 2^-24 in
// [0.5, 1): half the gap does not close it and twice the gap overshoots.
auto tConstantUnit = test("DepthBias/aConstantUnitIsTheDepthPrecisionThere") = []
{
    if (!Device::shared().isValid())
        return;

    check(probeWins(calibratedBy(calibrationUnits / 2.f)));
    check(!probeWins(calibratedBy(calibrationUnits * 2.f)));
};

// Encoder state on Metal: a pipeline without bias bound after one with it has
// to put the bias back to zero, or the coplanar probe is pushed behind.
auto tNoInheritance = test("DepthBias/anUnbiasedPipelineDoesNotInheritBias") = []
{
    if (!Device::shared().isValid())
        return;

    auto scene = Scene {};
    scene.lendBias = true;

    check(probeWins(scene));
};

// Slope-scaled bias scales with how steeply depth changes across the pixel:
// nothing on a quad facing the camera, enough on a tilted one to lose.
auto tSlopeScale = test("DepthBias/slopeScaleMovesATiltedQuadAndNotAFlatOne") = []
{
    if (!Device::shared().isValid())
        return;

    auto flat = Scene {};
    flat.probeBias.slopeScale = 1.f;
    check(probeWins(flat));

    auto tilted = flat;
    tilted.reference = tiltedQuad;
    tilted.probe = tiltedQuad;
    check(!probeWins(tilted));

    tilted.probeBias = {};
    check(probeWins(tilted));
};

// A probe 0.05 nearer and biased about 0.12 back loses, unless the clamp caps
// the bias short of the gap.
auto tClamp = test("DepthBias/aClampCapsTheBias") = []
{
    if (!Device::shared().isValid())
        return;

    auto scene = withConstant(4.0e6f);
    scene.probe = quadFrom(0.45f, 0.45f);
    check(!probeWins(scene));

    scene.probeBias.clamp = 0.01f;
    check(probeWins(scene));
};

// Without a depth test there is nothing for a bias to move, so the pipeline is
// built with none.
auto tIgnoredWithoutDepth = test("DepthBias/isIgnoredWithoutDepth") = []
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.depthBias = {8.f, 2.f, 0.5f};

    check(appliedDepthBias(descriptor).isNone());

    descriptor.depth = true;
    check(appliedDepthBias(descriptor).constant == 8.f);
    check(appliedDepthBias(descriptor).slopeScale == 2.f);
    check(appliedDepthBias(descriptor).clamp == 0.5f);
};
} // namespace DepthBiasTests
