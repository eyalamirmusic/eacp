#include "Common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

// Samplers declared as bindings of their own (Samplers.h) on the device: one
// compiled program, its behaviour decided by the Device::makeSampler sampler
// assigned to its member - filter, address mode, border, LOD clamp, mip
// filter and comparison function - in a fragment shader and, through the
// level, gradient, gather and comparison forms, in a kernel.
//
// Self-skips without a device or without per-bind samplers (the D3D12
// static-sampler fallback, see Lib/eacp/GPU/SAMPLERS.md).

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace SamplerBindingDeviceTests
{
namespace
{
struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace SamplerBindingDeviceTests

EACP_SHADER_VALUE(SamplerBindingDeviceTests::QuadVertex, Float2)

namespace SamplerBindingDeviceTests
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

constexpr auto stripWidth = 16;
constexpr auto stripHeight = 4;

bool canTest()
{
    auto& device = Device::shared();
    return device.isValid() && device.supportsPerBindSamplers();
}

bool near(float value, float expected)
{
    return std::abs(value - expected) < 0.003f;
}

std::uint32_t rgba(int red, int green, int blue)
{
    return 0xff000000u | ((std::uint32_t) blue << 16) | ((std::uint32_t) green << 8)
           | (std::uint32_t) red;
}

float unorm(int value)
{
    return (float) value / 255.f;
}

struct Expected
{
    float r;
    float g;
    float b;
    float a;
};

bool matches(const float* record, const Expected& expected)
{
    return near(record[0], expected.r) && near(record[1], expected.g)
           && near(record[2], expected.b) && near(record[3], expected.a);
}

constexpr auto redTexel = Expected {1.f, 0.f, 0.f, 1.f};
constexpr auto greenTexel = Expected {0.f, 1.f, 0.f, 1.f};
constexpr auto whiteTexel = Expected {1.f, 1.f, 1.f, 1.f};

SamplerDescriptor samplerWith(SamplerFilter filter, SamplerAddressMode address)
{
    auto descriptor = SamplerDescriptor {};
    descriptor.minFilter = filter;
    descriptor.magFilter = filter;
    descriptor.addressU = address;
    descriptor.addressV = address;
    descriptor.addressW = address;
    return descriptor;
}

SamplerDescriptor comparing(CompareFunction function)
{
    auto descriptor =
        samplerWith(SamplerFilter::Nearest, SamplerAddressMode::ClampToEdge);
    descriptor.compare = function;
    return descriptor;
}

Sampler makeSampler(const SamplerDescriptor& descriptor)
{
    return Device::shared().makeSampler(descriptor);
}

// Red at texel 0, green at texel 1.
Texture makeRedGreen()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 1;

    const std::uint32_t texels[] = {rgba(255, 0, 0), rgba(0, 255, 0)};
    return Device::shared().makeTexture(descriptor, texels);
}

Texture makeFloatTarget(int width, int height)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = TextureFormat::RGBA32Float;
    descriptor.renderTarget = true;
    return Device::shared().makeTexture(descriptor);
}

template <typename Shader>
void prepareShader(Shader& shader)
{
    shader.setVertices(fullQuad, 6);
    shader.prepare(1,
                   false,
                   PrimitiveTopology::Triangles,
                   BlendMode::None,
                   pixelFormatFor(TextureFormat::RGBA32Float));
}

struct FrameRunner final : GPUView
{
    FrameRunner() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        draw(frame);
        frame.beginPass({Graphics::Color::black()});
    }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

void runFrame(const std::function<void(Frame&)>& draw)
{
    auto view = FrameRunner {};
    view.draw = draw;
    view.setBounds({0.f, 0.f, 4.f, 4.f});
    check(view.renderToImage(1.f).isValid());
}

// One draw of `shader` into `target`, read back as floats.
template <typename Shader>
Vector<float> drawAndRead(
    Shader& shader,
    const Texture& target,
    const std::function<void(Frame&)>& before = [](Frame&) {})
{
    auto pixels = Vector<float> {};
    pixels.resize(target.width() * target.height() * 4);

    auto draw = [&](Frame& frame)
    {
        before(frame);

        {
            auto pass = frame.beginPass(target);
            pass.draw(shader);
        }

        frame.flush();
        target.read(pixels.data());
    };

    runFrame(draw);
    return pixels;
}

const float* pixelAt(const Vector<float>& pixels, int x)
{
    return pixels.data() + x * 4;
}

// u runs origin .. origin + span across the strip, v holds the texture's
// middle row.
struct StripShader final : ShaderProgram
{
    StripShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto across = fragmentCoordinate().x() / (float) stripWidth;
        auto uv = float2(origin + across * span, 0.5f);

        setFragment(sample(image, through, uv));
    }

    Uniform<Texture2D> image;
    Uniform<SamplerBinding> through;
    Uniform<Float> origin;
    Uniform<Float> span;

    EACP_SHADER(image, through, origin, span)
};

// The same texture through two samplers in one draw: red the nearest read's
// red, green the linear read's.
struct TwoWayShader final : ShaderProgram
{
    TwoWayShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto uv = float2(fragmentCoordinate().x() / (float) stripWidth, 0.5f);

        setFragment(float4(
            sample(image, crisp, uv).x(), sample(image, smooth, uv).x(), 0.f, 1.f));
    }

    Uniform<Texture2D> image;
    Uniform<SamplerBinding> crisp;
    Uniform<SamplerBinding> smooth;

    EACP_SHADER(image, crisp, smooth)
};

bool isPure(const float* pixel)
{
    return matches(pixel, redTexel) || matches(pixel, greenTexel);
}

bool isBlend(float channel)
{
    return channel > 0.05f && channel < 0.95f;
}

struct DepthWriter final : ShaderProgram
{
    DepthWriter()
    {
        compile();
        setVertices(fullQuad, 6);
        prepare(1,
                true,
                PrimitiveTopology::Triangles,
                BlendMode::None,
                pixelFormatFor(TextureFormat::RGBA8Unorm));
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position * extentX, depth, 1.f) + offset);
        setFragment(float4(depth, 0.f, 0.f, 1.f));
    }

    void place(float newDepth, bool leftHalf)
    {
        depth = newDepth;
        extentX = Array {leftHalf ? 0.5f : 1.f, 1.f};
        offset = Array {leftHalf ? -0.5f : 0.f, 0.f, 0.f, 0.f};
    }

    Uniform<Float> depth;
    Uniform<Float2> extentX;
    Uniform<Float4> offset;

    EACP_SHADER(depth, extentX, offset)
};

Texture makeDepthTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = 4;
    descriptor.height = 4;
    descriptor.renderTarget = true;
    descriptor.sampleableDepth = true;
    return Device::shared().makeTexture(descriptor);
}

// A 4x4 depth plane: 0.25 on its left half, 0.75 on its right.
struct DepthPlane
{
    DepthPlane()
    {
        back.place(0.75f, false);
        front.place(0.25f, true);
    }

    void draw(Frame& frame)
    {
        auto colour = ColorTarget {&target};

        auto descriptor = RenderPassDescriptor {};
        descriptor.depthAction = DepthAction::Keep;

        auto pass = frame.beginMultiTargetPass(
            std::span<const ColorTarget> {&colour, 1}, descriptor);
        pass.draw(back);
        pass.draw(front);
    }

    Texture target = makeDepthTarget();
    DepthWriter back;
    DepthWriter front;
};

// The plane's left and right halves compared with 0.5.
struct CompareShader final : ShaderProgram
{
    CompareShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto left = float2(constant(0.125f), 0.5f);
        auto right = float2(constant(0.875f), 0.5f);

        setFragment(float4(sampleCompare(depth, shadow, left, 0.5f),
                           sampleCompare(depth, shadow, right, 0.5f),
                           sample(depth, plain, right),
                           1.f));
    }

    Uniform<TextureDepth2D> depth;
    Uniform<ComparisonSamplerBinding> shadow;
    Uniform<SamplerBinding> plain;

    EACP_SHADER(depth, shadow, plain)
};

template <typename Kernel>
Vector<float> runKernel(
    Kernel& kernel,
    int records,
    const std::function<void(Frame&)>& before = [](Frame&) {})
{
    auto& device = Device::shared();
    auto output =
        device.makeBuffer((std::int64_t) records * 16, BufferUsage::Storage);
    kernel.output = output;
    kernel.prepare(device);
    check(kernel.pipeline().isValid());

    auto values = Vector<float> {};
    values.resize(records * 4);

    auto draw = [&](Frame& frame)
    {
        before(frame);

        {
            auto pass = frame.beginCompute();
            pass.dispatch(kernel, 1);
        }

        frame.flush();
        output.read(values.data(), (std::int64_t) records * 16);
    };

    runFrame(draw);
    return values;
}

constexpr auto levelledSize = 8;
constexpr auto levelledLevels = 4;

int levelRed(int level)
{
    return 40 * (level + 1);
}

Expected levelColour(int level)
{
    return {unorm(levelRed(level)), 0.f, 0.f, 1.f};
}

// Every level one solid colour whose red names the level.
Texture makeLevelled()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = levelledSize;
    descriptor.height = levelledSize;
    descriptor.mipLevels = levelledLevels;

    auto pixels = Vector<std::uint32_t> {};

    for (auto level = 0; level < levelledLevels; ++level)
    {
        const auto side = std::max(1, levelledSize >> level);

        for (auto i = 0; i < side * side; ++i)
            pixels.add(rgba(levelRed(level), 0, 0));
    }

    return Device::shared().makeTexture(descriptor, pixels.data());
}

// Level 2, level 0, and the gradients of a four-texel step (level 2), each
// through the one sampler.
struct LevelKernel final : ComputeProgram
{
    LevelKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        auto uv = float2(constant(0.3f), 0.3f);
        auto step = 4.f / (float) levelledSize;

        write(output, at, sampleLevel(image, through, uv, 2.f));
        write(output, at + 1u, sampleLevel(image, through, uv, 0.f));
        write(output,
              at + 2u,
              sampleGrad(image,
                         through,
                         uv,
                         float2(constant(step), 0.f),
                         float2(constant(0.f), step)));
    }

    Uniform<Texture2D> image;
    Uniform<SamplerBinding> through;
    Uniform<OutputBuffer> output;

    EACP_SHADER(image, through, output)
};

SamplerDescriptor mipmapped()
{
    auto descriptor =
        samplerWith(SamplerFilter::Nearest, SamplerAddressMode::Repeat);
    descriptor.mipFilter = SamplerMipFilter::Nearest;
    return descriptor;
}

// The four texels around the texture's top-left corner, which only the
// address mode decides.
struct CornerKernel final : ComputeProgram
{
    CornerKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        write(output, at, gather(image, through, float2(constant(0.f), 0.f)));
    }

    Uniform<Texture2D> image;
    Uniform<SamplerBinding> through;
    Uniform<OutputBuffer> output;

    EACP_SHADER(image, through, output)
};

// The any-stage comparisons: the plane's halves against 0.5, and the four
// texels around its centre.
struct CompareKernel final : ComputeProgram
{
    CompareKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        auto left = float2(constant(0.125f), 0.5f);
        auto right = float2(constant(0.875f), 0.5f);
        auto centre = float2(constant(0.5f), 0.5f);

        write(output,
              at,
              float4(sampleCompareLevel(depth, shadow, left, 0.5f),
                     sampleCompareLevel(depth, shadow, right, 0.5f),
                     sampleLevel(depth, plain, left, 0),
                     1.f));
        write(output, at + 1u, gatherCompare(depth, shadow, centre, 0.5f));
    }

    Uniform<TextureDepth2D> depth;
    Uniform<ComparisonSamplerBinding> shadow;
    Uniform<SamplerBinding> plain;
    Uniform<OutputBuffer> output;

    EACP_SHADER(depth, shadow, plain, output)
};
} // namespace

auto tFilter = test("SamplerBindingDevice/theBoundSamplerDecidesTheFilter") = []
{
    if (!canTest())
        return;

    auto image = makeRedGreen();
    auto target = makeFloatTarget(stripWidth, stripHeight);

    auto shader = StripShader {};
    shader.image = image;
    shader.origin = 0.f;
    shader.span = 1.f;
    prepareShader(shader);

    shader.through = makeSampler(
        samplerWith(SamplerFilter::Nearest, SamplerAddressMode::ClampToEdge));
    const auto nearest = drawAndRead(shader, target);

    shader.through = makeSampler(
        samplerWith(SamplerFilter::Linear, SamplerAddressMode::ClampToEdge));
    const auto linear = drawAndRead(shader, target);

    auto allPure = true;
    auto anyBlend = false;

    for (auto x = 0; x < stripWidth; ++x)
    {
        allPure = allPure && isPure(pixelAt(nearest, x));
        anyBlend = anyBlend || isBlend(pixelAt(linear, x)[0]);
    }

    check(allPure);
    check(anyBlend);
    check(matches(pixelAt(nearest, 0), redTexel));
    check(matches(pixelAt(nearest, stripWidth - 1), greenTexel));
};

// u runs 1..2, entirely outside the texture, where only the address mode
// says what is read.
auto tAddress =
    test("SamplerBindingDevice/theBoundSamplerDecidesTheAddressMode") = []
{
    if (!canTest())
        return;

    auto image = makeRedGreen();
    auto target = makeFloatTarget(stripWidth, stripHeight);

    auto shader = StripShader {};
    shader.image = image;
    shader.origin = 1.f;
    shader.span = 1.f;
    prepareShader(shader);

    auto readThrough = [&](SamplerAddressMode address)
    {
        shader.through = makeSampler(samplerWith(SamplerFilter::Nearest, address));
        return drawAndRead(shader, target);
    };

    const auto clamped = readThrough(SamplerAddressMode::ClampToEdge);
    const auto repeated = readThrough(SamplerAddressMode::Repeat);
    const auto mirrored = readThrough(SamplerAddressMode::MirrorRepeat);

    auto border =
        samplerWith(SamplerFilter::Nearest, SamplerAddressMode::ClampToBorder);
    border.borderColor = SamplerBorderColor::OpaqueWhite;
    shader.through = makeSampler(border);
    const auto bordered = drawAndRead(shader, target);

    const auto left = 2;
    const auto right = stripWidth - 3;

    check(matches(pixelAt(clamped, left), greenTexel));
    check(matches(pixelAt(clamped, right), greenTexel));
    check(matches(pixelAt(repeated, left), redTexel));
    check(matches(pixelAt(repeated, right), greenTexel));
    check(matches(pixelAt(mirrored, left), greenTexel));
    check(matches(pixelAt(mirrored, right), redTexel));
    check(matches(pixelAt(bordered, left), whiteTexel));
    check(matches(pixelAt(bordered, right), whiteTexel));
};

auto tTwoWays = test("SamplerBindingDevice/twoSamplersReadOneTextureInOneDraw") = []
{
    if (!canTest())
        return;

    auto image = makeRedGreen();
    auto target = makeFloatTarget(stripWidth, stripHeight);

    auto shader = TwoWayShader {};
    shader.image = image;
    shader.crisp = makeSampler(
        samplerWith(SamplerFilter::Nearest, SamplerAddressMode::ClampToEdge));
    shader.smooth = makeSampler(
        samplerWith(SamplerFilter::Linear, SamplerAddressMode::ClampToEdge));
    prepareShader(shader);

    const auto pixels = drawAndRead(shader, target);

    auto crispIsPure = true;
    auto smoothBlends = false;

    for (auto x = 0; x < stripWidth; ++x)
    {
        const auto* pixel = pixelAt(pixels, x);
        crispIsPure = crispIsPure && (near(pixel[0], 0.f) || near(pixel[0], 1.f));
        smoothBlends = smoothBlends || isBlend(pixel[1]);
    }

    check(crispIsPure);
    check(smoothBlends);
};

// Reference 0.5 against 0.25 on the left and 0.75 on the right: Less passes
// only on the right, Greater only on the left. The plain sampler beside it
// reads the stored depth.
auto tCompare = test("SamplerBindingDevice/theBoundSamplerDecidesTheComparison") = []
{
    if (!canTest())
        return;

    auto plane = DepthPlane {};
    check(plane.target.hasSampleableDepth());

    auto target = makeFloatTarget(4, 4);
    auto shader = CompareShader {};
    shader.depth = plane.target;
    shader.plain = makeSampler(
        samplerWith(SamplerFilter::Nearest, SamplerAddressMode::ClampToEdge));
    prepareShader(shader);

    auto drawPlane = [&](Frame& frame) { plane.draw(frame); };

    shader.shadow = makeSampler(comparing(CompareFunction::Less));
    const auto less = drawAndRead(shader, target, drawPlane);

    shader.shadow = makeSampler(comparing(CompareFunction::Greater));
    const auto greater = drawAndRead(shader, target, drawPlane);

    check(matches(pixelAt(less, 0), {0.f, 1.f, 0.75f, 1.f}));
    check(matches(pixelAt(greater, 0), {1.f, 0.f, 0.75f, 1.f}));
};

// Levels 2 and 0 and a level-2 gradient, through samplers that clamp the
// level or take no mip at all.
auto tLevels = test("SamplerBindingDevice/aKernelsSamplerClampsTheLevelItReads") = []
{
    if (!canTest())
        return;

    auto image = makeLevelled();
    auto kernel = LevelKernel {};
    kernel.image = image;

    auto readThrough = [&](const SamplerDescriptor& descriptor)
    {
        kernel.through = makeSampler(descriptor);
        return runKernel(kernel, 3);
    };

    const auto unclamped = readThrough(mipmapped());

    auto capped = mipmapped();
    capped.lodMaxClamp = 1.f;
    const auto clampedHigh = readThrough(capped);

    auto floored = mipmapped();
    floored.lodMinClamp = 1.f;
    const auto clampedLow = readThrough(floored);

    auto single = mipmapped();
    single.mipFilter = SamplerMipFilter::None;
    const auto unmipped = readThrough(single);

    check(matches(unclamped.data(), levelColour(2)), "level 2");
    check(matches(unclamped.data() + 4, levelColour(0)), "level 0");
    check(matches(unclamped.data() + 8, levelColour(2)), "gradient");

    check(matches(clampedHigh.data(), levelColour(1)), "capped level 2");
    check(matches(clampedHigh.data() + 4, levelColour(0)), "capped level 0");
    check(matches(clampedHigh.data() + 8, levelColour(1)), "capped gradient");

    check(matches(clampedLow.data(), levelColour(2)), "floored level 2");
    check(matches(clampedLow.data() + 4, levelColour(1)), "floored level 0");

    check(matches(unmipped.data(), levelColour(0)), "unmipped level 2");
    check(matches(unmipped.data() + 8, levelColour(0)), "unmipped gradient");
};

// At the corner the footprint lies half outside: a repeating sampler gathers
// the texels of the far row and column, a clamping one the corner four times.
auto tGather = test("SamplerBindingDevice/aKernelsGatherWrapsAsItsSamplerSays") = []
{
    if (!canTest())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;

    const std::uint32_t texels[] = {
        rgba(10, 0, 0), rgba(20, 0, 0), rgba(30, 0, 0), rgba(40, 0, 0)};
    auto image = Device::shared().makeTexture(descriptor, texels);

    auto kernel = CornerKernel {};
    kernel.image = image;

    kernel.through =
        makeSampler(samplerWith(SamplerFilter::Nearest, SamplerAddressMode::Repeat));
    const auto repeated = runKernel(kernel, 1);

    kernel.through = makeSampler(
        samplerWith(SamplerFilter::Nearest, SamplerAddressMode::ClampToEdge));
    const auto clamped = runKernel(kernel, 1);

    check(matches(repeated.data(), {unorm(20), unorm(10), unorm(30), unorm(40)}));
    check(matches(clamped.data(), {unorm(10), unorm(10), unorm(10), unorm(10)}));
};

auto tKernelCompare =
    test("SamplerBindingDevice/aKernelsComparisonsFollowTheBoundFunction") = []
{
    if (!canTest())
        return;

    auto plane = DepthPlane {};
    auto kernel = CompareKernel {};
    kernel.depth = plane.target;
    kernel.plain = makeSampler(
        samplerWith(SamplerFilter::Nearest, SamplerAddressMode::ClampToEdge));

    auto drawPlane = [&](Frame& frame) { plane.draw(frame); };

    kernel.shadow = makeSampler(comparing(CompareFunction::Less));
    const auto less = runKernel(kernel, 2, drawPlane);

    kernel.shadow = makeSampler(comparing(CompareFunction::Greater));
    const auto greater = runKernel(kernel, 2, drawPlane);

    check(matches(less.data(), {0.f, 1.f, 0.25f, 1.f}));
    check(matches(less.data() + 4, {0.f, 1.f, 1.f, 0.f}));
    check(matches(greater.data(), {1.f, 0.f, 0.25f, 1.f}));
    check(matches(greater.data() + 4, {1.f, 0.f, 0.f, 1.f}));
};

// A member binds only a sampler of its own kind; anything else binds nothing.
auto tMemberRule =
    test("SamplerBindingDevice/aMemberBindsOnlyItsOwnKindOfSampler") = []
{
    if (!canTest())
        return;

    const auto plain =
        makeSampler(samplerWith(SamplerFilter::Linear, SamplerAddressMode::Repeat));
    const auto shadow = makeSampler(comparing(CompareFunction::LessEqual));

    check(detail::fitsSamplerMember("plain", plain, false));
    check(detail::fitsSamplerMember("shadow", shadow, true));
    check(!detail::fitsSamplerMember("plain", shadow, false));
    check(!detail::fitsSamplerMember("shadow", plain, true));
    check(!detail::fitsSamplerMember("unassigned", Sampler {}, false));
};
} // namespace SamplerBindingDeviceTests
