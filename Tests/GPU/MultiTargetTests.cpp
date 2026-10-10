#include "Common.h"

#include <cmath>
#include <cstring>
#include <functional>

// Frame::beginMultiTargetPass and RenderPipelineDescriptor::extraColorTargets:
// one draw writing several textures at once, each through its own format,
// blend and clear. The shaders are hand-written, the way bgfx's are, with one
// output per attachment: target n is written from the vertex colour c as
// output(c, n) below says, so every pixel read back has an expected value.
//
// Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto targetSize = 4;

struct ColorVertex
{
    float position[3];
    float color[4];
};

// What attachment n holds for a fragment of colour c: c itself, c with red
// and blue swapped, and red plus twice green in a single channel.
std::string mslOutput(int index)
{
    if (index == 0)
        return "in.color";

    if (index == 1)
        return "in.color.zyxw";

    return "float4(in.color.x + 2.0 * in.color.y, 0.0, 0.0, 1.0)";
}

std::string mslSource(int count)
{
    auto outputs = std::string {};
    auto writes = std::string {};

    for (auto index = 0; index < count; ++index)
    {
        const auto name = "c" + std::to_string(index);
        outputs +=
            "    float4 " + name + " [[color(" + std::to_string(index) + ")]];\n";
        writes += "    out." + name + " = " + mslOutput(index) + ";\n";
    }

    return R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float4 color;
};

struct FragmentOut
{
)" + outputs
           + R"(};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 1.0);
    out.color = in.color;
    return out;
}

fragment FragmentOut fragmentMain(VertexOut in [[stage_in]])
{
    FragmentOut out;
)" + writes + R"(    return out;
}
)";
}

std::string hlslSource(int count)
{
    auto outputs = std::string {};
    auto writes = std::string {};

    for (auto index = 0; index < count; ++index)
    {
        const auto name = "c" + std::to_string(index);
        outputs +=
            "    float4 " + name + " : SV_Target" + std::to_string(index) + ";\n";

        auto value = mslOutput(index);
        auto at = value.find("in.");

        while (at != std::string::npos)
        {
            value.replace(at, 3, "input.");
            at = value.find("in.", at + 6);
        }

        writes += "    o." + name + " = " + value + ";\n";
    }

    return R"(
struct VertexIn
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

struct FragmentOut
{
)" + outputs
           + R"(};

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 1.0);
    o.color = input.color;
    return o;
}

FragmentOut fragmentMain(VertexOut input)
{
    FragmentOut o;
)" + writes + R"(    return o;
}
)";
}

std::string glslSource(int count)
{
    auto outputs = std::string {};
    auto writes = std::string {};

    for (auto index = 0; index < count; ++index)
    {
        const auto name = "c" + std::to_string(index);
        outputs += "layout(location = " + std::to_string(index) + ") out vec4 "
                   + name + ";\n";

        auto value = mslOutput(index);
        auto at = value.find("in.color");

        while (at != std::string::npos)
        {
            value.replace(at, 8, "color");
            at = value.find("in.color", at);
        }

        at = value.find("float4");

        if (at != std::string::npos)
            value.replace(at, 6, "vec4");

        writes += "    " + name + " = " + value + ";\n";
    }

    return R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec3 attr0;
layout(location = 1) in vec4 attr1;
layout(location = 0) out vec4 color;

void main()
{
    gl_Position = vec4(attr0, 1.0);
    color = attr1;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec4 color;
)" + outputs
           + R"(
void main()
{
)" + writes + R"(}
#endif
)";
}

ShaderLibrary makeLibrary(int outputs)
{
    auto source = nativeShaderSource(
        mslSource(outputs), hlslSource(outputs), glslSource(outputs));

    return Device::shared().makeShaderLibrary(
        source.withVertex("vertexMain").withFragment("fragmentMain"));
}

ColorTargetState opaque(PixelFormat format)
{
    auto state = ColorTargetState {};
    state.format = format;
    return state;
}

RenderPipeline makePipeline(const ShaderLibrary& library,
                            const Vector<ColorTargetState>& targets,
                            bool depth = false,
                            int sampleCount = 1)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.depth = depth;
    descriptor.sampleCount = sampleCount;
    descriptor.vertexLayout.attribute(VertexFormat::Float3, 0);
    descriptor.vertexLayout.attribute(VertexFormat::Float4, 12);
    descriptor.vertexLayout.stride = sizeof(ColorVertex);

    descriptor.colorFormat = targets[0].format;
    descriptor.blendMode = targets[0].blendMode;
    descriptor.blend = targets[0].blend;
    descriptor.colorWriteMask = targets[0].writeMask;

    for (auto index = 1; index < (int) targets.size(); ++index)
        descriptor.extraColorTargets.add(targets[index]);

    return Device::shared().makeRenderPipeline(descriptor);
}

Texture makeTarget(TextureFormat format,
                   int size = targetSize,
                   bool depth = false,
                   int sampleCount = 1)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.format = format;
    descriptor.renderTarget = true;
    descriptor.depth = depth;
    descriptor.sampleCount = sampleCount;

    return Device::shared().makeTexture(descriptor);
}

// A quad over x in [left, right] at depth z, every vertex the one colour.
Buffer makeQuad(float left, float right, float z, const float (&color)[4])
{
    const auto corner = [&](float x, float y)
    { return ColorVertex {{x, y, z}, {color[0], color[1], color[2], color[3]}}; };

    const ColorVertex vertices[] = {corner(left, -1.f),
                                    corner(right, -1.f),
                                    corner(left, 1.f),
                                    corner(right, -1.f),
                                    corner(right, 1.f),
                                    corner(left, 1.f)};

    return Device::shared().makeBuffer(vertices, sizeof(vertices));
}

Buffer makeFullQuad(const float (&color)[4], float z = 0.5f)
{
    return makeQuad(-1.f, 1.f, z, color);
}

// One frame, rendered off-screen, whose passes are whatever `draw` records.
// The drawable gets a pass of its own after them, since a frame with nothing
// to present has nothing for the snapshot to hand back.
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

void runFrame(std::function<void(Frame&)> draw)
{
    auto view = FrameRunner {};
    view.draw = std::move(draw);
    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    view.renderToImage(1.f);
}

using Rgba8 = Array<unsigned char, 4>;

Vector<Rgba8> readRgba8(const Texture& texture)
{
    auto bytes = Vector<unsigned char> {};
    bytes.resize((std::size_t) (texture.width() * texture.height() * 4));
    texture.read(bytes.data());

    auto pixels = Vector<Rgba8> {};

    for (auto index = 0; index < texture.width() * texture.height(); ++index)
        pixels.add({bytes[index * 4],
                    bytes[index * 4 + 1],
                    bytes[index * 4 + 2],
                    bytes[index * 4 + 3]});

    return pixels;
}

bool matches(const Rgba8& pixel, int r, int g, int b, int a = 255)
{
    const auto close = [](int value, int expected)
    { return value >= expected - 2 && value <= expected + 2; };

    return close(pixel[0], r) && close(pixel[1], g) && close(pixel[2], b)
           && close(pixel[3], a);
}

float halfToFloat(std::uint16_t half)
{
    const auto sign = (half & 0x8000) != 0 ? -1.f : 1.f;
    const auto exponent = (half >> 10) & 0x1f;
    const auto mantissa = half & 0x3ff;

    if (exponent == 0)
        return sign * std::ldexp((float) mantissa, -24);

    return sign * std::ldexp((float) (mantissa | 0x400), exponent - 25);
}

Array<float, 4> readRgba16FloatAt(const Texture& texture, int x, int y)
{
    auto halves = Vector<std::uint16_t> {};
    halves.resize((std::size_t) (texture.width() * texture.height() * 4));
    texture.read(halves.data());

    const auto at = (y * texture.width() + x) * 4;

    return {halfToFloat(halves[at]),
            halfToFloat(halves[at + 1]),
            halfToFloat(halves[at + 2]),
            halfToFloat(halves[at + 3])};
}

float readR32FloatAt(const Texture& texture, int x, int y)
{
    auto values = Vector<float> {};
    values.resize((std::size_t) (texture.width() * texture.height()));
    texture.read(values.data());

    return values[y * texture.width() + x];
}

Texture makeLayeredTarget(TextureType kind, int layers, bool depth = false)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.kind = kind;
    descriptor.arrayLength = layers;
    descriptor.renderTarget = true;
    descriptor.depth = depth;

    return Device::shared().makeTexture(descriptor);
}

Vector<Rgba8> readSlice(const Texture& texture, int slice)
{
    auto pixels = Vector<Rgba8> {};
    pixels.resize((std::size_t) (texture.width() * texture.height()));
    texture.readSubresource(TextureRegion::wholeLevel(0, slice), pixels.data());
    return pixels;
}

ColorTarget sliceOf(const Texture& texture, int slice, bool clear = true)
{
    auto target = ColorTarget {&texture};
    target.slice = slice;
    target.clear = clear;
    target.clearColor = {0.5f, 0.5f, 0.5f, 1.f};
    return target;
}

void clearSlices(Frame& frame, const Texture& texture)
{
    for (auto slice = 0; slice < texture.slices(); ++slice)
    {
        const auto target = sliceOf(texture, slice);
        frame.beginMultiTargetPass(std::span<const ColorTarget> {&target, 1});
    }
}

bool canDraw()
{
    return Device::shared().isValid();
}
} // namespace

// The minimal case: one draw, two textures, a different value in each.
auto tTwoTargets = test("MultiTarget/twoTargetsTakeTheirOwnOutputs") = []
{
    if (!canDraw())
        return;

    auto first = makeTarget(TextureFormat::RGBA8Unorm);
    auto second = makeTarget(TextureFormat::RGBA8Unorm);
    auto library = makeLibrary(2);
    auto pipeline = makePipeline(
        library, {opaque(PixelFormat::RGBA8Unorm), opaque(PixelFormat::RGBA8Unorm)});

    check(pipeline.isValid());

    constexpr float color[] = {1.f, 0.5f, 0.f, 1.f};
    auto quad = makeFullQuad(color);

    runFrame(
        [&](Frame& frame)
        {
            const ColorTarget targets[] = {{&first}, {&second}};
            auto pass = frame.beginMultiTargetPass(targets);

            check(pass.targetWidth() == targetSize);

            pass.setPipeline(pipeline);
            pass.setVertexBuffer(quad);
            pass.draw(6);
        });

    for (const auto& pixel: readRgba8(first))
        check(matches(pixel, 255, 128, 0));

    for (const auto& pixel: readRgba8(second))
        check(matches(pixel, 0, 128, 255));
};

// Every attachment clears to the colour its ColorTarget names, and one that
// asks to load keeps what an earlier pass left.
auto tPerTargetClears = test("MultiTarget/eachTargetClearsOrLoadsOnItsOwn") = []
{
    if (!canDraw())
        return;

    auto first = makeTarget(TextureFormat::RGBA8Unorm);
    auto second = makeTarget(TextureFormat::RGBA8Unorm);
    auto third = makeTarget(TextureFormat::RGBA8Unorm);

    runFrame(
        [&](Frame& frame)
        {
            frame.beginPass(third, {{0.f, 0.f, 1.f, 1.f}});

            const ColorTarget targets[] = {
                {&first, {1.f, 0.f, 0.f, 1.f}},
                {&second, {0.f, 1.f, 0.f, 1.f}},
                {&third, {1.f, 1.f, 1.f, 1.f}, false},
            };

            frame.beginMultiTargetPass(targets);
        });

    check(matches(readRgba8(first)[0], 255, 0, 0));
    check(matches(readRgba8(second)[0], 0, 255, 0));
    check(matches(readRgba8(third)[0], 0, 0, 255));
};

// The overload over plain textures clears them all as the descriptor says,
// which is bgfx's one clear colour per framebuffer.
auto tTextureListClearsAlike = test("MultiTarget/aTextureListClearsAlike") = []
{
    if (!canDraw())
        return;

    auto first = makeTarget(TextureFormat::RGBA8Unorm);
    auto second = makeTarget(TextureFormat::RGBA8Unorm);

    runFrame(
        [&](Frame& frame)
        {
            const Texture* targets[] = {&first, &second};
            frame.beginMultiTargetPass(targets, {{0.f, 1.f, 1.f, 1.f}});
        });

    check(matches(readRgba8(first)[0], 0, 255, 255));
    check(matches(readRgba8(second)[0], 0, 255, 255));
};

// Target 0 adds onto its clear, target 1 replaces its clear, in one draw: the
// blend is the attachment's, not the pipeline's.
auto tIndependentBlend = test("MultiTarget/eachTargetBlendsOnItsOwn") = []
{
    if (!canDraw())
        return;

    auto first = makeTarget(TextureFormat::RGBA8Unorm);
    auto second = makeTarget(TextureFormat::RGBA8Unorm);
    auto library = makeLibrary(2);

    auto additive = opaque(PixelFormat::RGBA8Unorm);
    additive.blend = BlendState {true,
                                 BlendFactor::One,
                                 BlendFactor::One,
                                 BlendOperation::Add,
                                 BlendFactor::Zero,
                                 BlendFactor::One,
                                 BlendOperation::Add};

    auto pipeline =
        makePipeline(library, {additive, opaque(PixelFormat::RGBA8Unorm)});

    check(pipeline.isValid());

    constexpr float color[] = {0.5f, 0.25f, 0.125f, 1.f};
    auto quad = makeFullQuad(color);

    runFrame(
        [&](Frame& frame)
        {
            const ColorTarget targets[] = {
                {&first, {0.25f, 0.25f, 0.25f, 1.f}},
                {&second, {0.25f, 0.25f, 0.25f, 1.f}},
            };

            auto pass = frame.beginMultiTargetPass(targets);
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(quad);
            pass.draw(6);
        });

    check(matches(readRgba8(first)[0], 191, 128, 96));
    check(matches(readRgba8(second)[0], 32, 64, 128));
};

// A write mask is per attachment too: the second target keeps its clear in
// every channel but red.
auto tIndependentWriteMask = test("MultiTarget/eachTargetMasksOnItsOwn") = []
{
    if (!canDraw())
        return;

    auto first = makeTarget(TextureFormat::RGBA8Unorm);
    auto second = makeTarget(TextureFormat::RGBA8Unorm);
    auto library = makeLibrary(2);

    auto redOnly = opaque(PixelFormat::RGBA8Unorm);
    redOnly.writeMask = {true, false, false, false};

    auto pipeline =
        makePipeline(library, {opaque(PixelFormat::RGBA8Unorm), redOnly});

    check(pipeline.isValid());

    constexpr float color[] = {0.f, 1.f, 1.f, 1.f};
    auto quad = makeFullQuad(color);

    runFrame(
        [&](Frame& frame)
        {
            const ColorTarget targets[] = {{&first},
                                           {&second, {0.f, 0.f, 0.f, 1.f}}};

            auto pass = frame.beginMultiTargetPass(targets);
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(quad);
            pass.draw(6);
        });

    check(matches(readRgba8(first)[0], 0, 255, 255));
    check(matches(readRgba8(second)[0], 255, 0, 0));
};

// Three attachments in three formats, each keeping what its format can: the
// 8-bit one clamps 2.5 to 1, the half-float one keeps it, and the single
// float channel holds a value no colour format would.
auto tMixedFormats = test("MultiTarget/threeTargetsInThreeFormats") = []
{
    if (!canDraw())
        return;

    auto bytes = makeTarget(TextureFormat::RGBA8Unorm);
    auto halves = makeTarget(TextureFormat::RGBA16Float);
    auto single = makeTarget(TextureFormat::R32Float);
    auto library = makeLibrary(3);
    auto pipeline = makePipeline(library,
                                 {opaque(PixelFormat::RGBA8Unorm),
                                  opaque(PixelFormat::RGBA16Float),
                                  opaque(PixelFormat::R32Float)});

    check(pipeline.isValid());

    constexpr float color[] = {2.5f, 0.5f, 0.25f, 1.f};
    auto quad = makeFullQuad(color);

    runFrame(
        [&](Frame& frame)
        {
            const ColorTarget targets[] = {{&bytes}, {&halves}, {&single}};

            auto pass = frame.beginMultiTargetPass(targets);
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(quad);
            pass.draw(6);
        });

    check(matches(readRgba8(bytes)[5], 255, 128, 64));

    const auto half = readRgba16FloatAt(halves, 1, 1);

    check(std::abs(half[0] - 0.25f) < 0.01f);
    check(std::abs(half[1] - 0.5f) < 0.01f);
    check(std::abs(half[2] - 2.5f) < 0.01f);

    check(std::abs(readR32FloatAt(single, 2, 2) - 3.5f) < 0.001f);
};

// Depth comes from the first target and tests every attachment's fragments
// alike: the near quad on the left survives a far one drawn over everything
// after it, in both textures.
auto tDepthOnAMultiTargetPass = test("MultiTarget/depthTestsEveryTarget") = []
{
    if (!canDraw())
        return;

    auto first = makeTarget(TextureFormat::RGBA8Unorm, targetSize, true);
    auto second = makeTarget(TextureFormat::RGBA8Unorm);
    auto library = makeLibrary(2);
    auto pipeline = makePipeline(
        library,
        {opaque(PixelFormat::RGBA8Unorm), opaque(PixelFormat::RGBA8Unorm)},
        true);

    check(first.hasDepth());
    check(pipeline.isValid());

    constexpr float nearColor[] = {1.f, 0.f, 0.f, 1.f};
    constexpr float farColor[] = {0.f, 1.f, 0.f, 1.f};
    auto nearQuad = makeQuad(-1.f, 0.f, 0.25f, nearColor);
    auto farQuad = makeFullQuad(farColor, 0.75f);

    runFrame(
        [&](Frame& frame)
        {
            const ColorTarget targets[] = {{&first}, {&second}};

            auto pass = frame.beginMultiTargetPass(targets);
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(nearQuad);
            pass.draw(6);
            pass.setVertexBuffer(farQuad);
            pass.draw(6);
        });

    const auto firstPixels = readRgba8(first);
    const auto secondPixels = readRgba8(second);
    const auto row = targetSize * (targetSize / 2);

    check(matches(firstPixels[row], 255, 0, 0));
    check(matches(firstPixels[row + targetSize - 1], 0, 255, 0));
    check(matches(secondPixels[row], 0, 0, 255));
    check(matches(secondPixels[row + targetSize - 1], 0, 255, 0));
};

// Multisampled targets render into their sample textures and every one of
// them resolves into itself as the pass ends.
auto tMultisampledTargets = test("MultiTarget/multisampledTargetsAllResolve") = []
{
    if (!canDraw() || !Device::shared().supportsSampleCount(4))
        return;

    auto first = makeTarget(TextureFormat::RGBA8Unorm, targetSize, false, 4);
    auto second = makeTarget(TextureFormat::RGBA8Unorm, targetSize, false, 4);
    auto library = makeLibrary(2);
    auto pipeline = makePipeline(
        library,
        {opaque(PixelFormat::RGBA8Unorm), opaque(PixelFormat::RGBA8Unorm)},
        false,
        4);

    check(pipeline.isValid());

    constexpr float color[] = {0.f, 0.f, 1.f, 1.f};
    auto quad = makeFullQuad(color);

    runFrame(
        [&](Frame& frame)
        {
            const ColorTarget targets[] = {{&first}, {&second}};

            auto pass = frame.beginMultiTargetPass(targets);
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(quad);
            pass.draw(6);
        });

    check(matches(readRgba8(first)[6], 0, 0, 255));
    check(matches(readRgba8(second)[6], 255, 0, 0));
};

// Targets that disagree in size or sample count, a texture that is not a
// render target, one image of a texture twice, none at all and too many are
// each refused: the pass records nothing, so the target keeps the red an
// earlier pass left.
auto tMismatchRefused = test("MultiTarget/mismatchedTargetsAreRefused") = []
{
    if (!canDraw())
        return;

    auto first = makeTarget(TextureFormat::RGBA8Unorm);
    auto larger = makeTarget(TextureFormat::RGBA8Unorm, targetSize * 2);

    auto plainDescriptor = TextureDescriptor {};
    plainDescriptor.width = targetSize;
    plainDescriptor.height = targetSize;
    auto plain = Device::shared().makeTexture(plainDescriptor);

    auto refusals = Vector<int> {};

    runFrame(
        [&](Frame& frame)
        {
            frame.beginPass(first, {{1.f, 0.f, 0.f, 1.f}});

            const auto attempt = [&](std::span<const ColorTarget> targets)
            {
                auto pass = frame.beginMultiTargetPass(targets);
                refusals.add(pass.targetWidth());
            };

            const Graphics::Color green = {0.f, 1.f, 0.f, 1.f};

            const ColorTarget sizes[] = {{&first, green}, {&larger, green}};
            const ColorTarget notTarget[] = {{&first, green}, {&plain, green}};
            const ColorTarget twice[] = {{&first, green}, {&first, green}};
            const ColorTarget nullTexture[] = {{&first, green}, {nullptr, green}};

            attempt(sizes);
            attempt(notTarget);
            attempt(twice);
            attempt(nullTexture);
            attempt({});

            auto tooMany = Array<ColorTarget, maxColorAttachments + 1> {};

            for (auto& target: tooMany)
                target = {&first, green};

            attempt(tooMany);
        });

    check(refusals.size() == 6);

    for (auto width: refusals)
        check(width == 0);

    check(matches(readRgba8(first)[0], 255, 0, 0));
};

// A sample-count mismatch is refused the same way.
auto tSampleCountMismatchRefused =
    test("MultiTarget/mismatchedSampleCountsAreRefused") = []
{
    if (!canDraw() || !Device::shared().supportsSampleCount(4))
        return;

    auto single = makeTarget(TextureFormat::RGBA8Unorm);
    auto multi = makeTarget(TextureFormat::RGBA8Unorm, targetSize, false, 4);
    auto width = -1;

    runFrame(
        [&](Frame& frame)
        {
            const ColorTarget targets[] = {{&single}, {&multi}};
            width = frame.beginMultiTargetPass(targets).targetWidth();
        });

    check(width == 0);
};

// A pipeline naming more attachments than any pass can have is refused at
// creation rather than at the draw.
auto tTooManyPipelineTargets =
    test("MultiTarget/aPipelineBeyondTheLimitIsInvalid") = []
{
    if (!canDraw())
        return;

    auto library = makeLibrary(2);
    auto targets = Vector<ColorTargetState> {};

    for (auto index = 0; index <= maxColorAttachments; ++index)
        targets.add(opaque(PixelFormat::RGBA8Unorm));

    check(!makePipeline(library, targets).isValid());
};

// Layers 0 and 2 of one array are the pass's two targets, each taking its own
// output, and layer 1 between them keeps the grey it was cleared to. The same
// layer twice, or two runs that share one, is still refused.
auto tLayersOfOneArray = test("MultiTarget/twoLayersOfOneArrayAreTwoTargets") = []
{
    if (!canDraw())
        return;

    auto array = makeLayeredTarget(TextureType::TwoDArray, 3);
    auto library = makeLibrary(2);
    auto pipeline = makePipeline(
        library, {opaque(PixelFormat::RGBA8Unorm), opaque(PixelFormat::RGBA8Unorm)});

    check(array.isValid() && pipeline.isValid());

    if (!array.isValid())
        return;

    constexpr float color[] = {1.f, 0.5f, 0.f, 1.f};
    auto quad = makeFullQuad(color);
    auto widths = Vector<int> {};

    runFrame(
        [&](Frame& frame)
        {
            clearSlices(frame, array);

            const ColorTarget targets[] = {sliceOf(array, 0, false),
                                           sliceOf(array, 2, false)};
            auto pass = frame.beginMultiTargetPass(targets);
            widths.add(pass.targetWidth());
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(quad);
            pass.draw(6);
            pass.end();

            const ColorTarget sameLayer[] = {sliceOf(array, 1), sliceOf(array, 1)};
            widths.add(frame.beginMultiTargetPass(sameLayer).targetWidth());

            ColorTarget sharedRun[] = {sliceOf(array, 0), sliceOf(array, 1)};
            sharedRun[0].layerCount = 2;
            sharedRun[1].layerCount = 2;
            widths.add(frame.beginMultiTargetPass(sharedRun).targetWidth());
        });

    check(widths.size() == 3);

    if (widths.size() == 3)
    {
        check(widths[0] == targetSize);
        check(widths[1] == 0);
        check(widths[2] == 0);
    }

    for (const auto& pixel: readSlice(array, 0))
        check(matches(pixel, 255, 128, 0));

    for (const auto& pixel: readSlice(array, 1))
        check(matches(pixel, 128, 128, 128));

    for (const auto& pixel: readSlice(array, 2))
        check(matches(pixel, 0, 128, 255));
};

// Two faces of one cube as two targets, the depth plane the first face's: the
// near half of each face holds the near quad's output and the far half the
// far quad's, so both attachments were tested against the one plane.
auto tFacesOfOneCube = test("MultiTarget/twoFacesOfOneCubeAreTwoTargets") = []
{
    if (!canDraw())
        return;

    auto cube = makeLayeredTarget(TextureType::Cube, 1, true);
    auto library = makeLibrary(2);
    auto pipeline = makePipeline(
        library,
        {opaque(PixelFormat::RGBA8Unorm), opaque(PixelFormat::RGBA8Unorm)},
        true);

    check(cube.isValid() && cube.hasDepth() && pipeline.isValid());

    if (!cube.isValid())
        return;

    constexpr float nearColor[] = {1.f, 0.f, 0.f, 1.f};
    constexpr float farColor[] = {0.f, 1.f, 0.f, 1.f};
    auto nearQuad = makeQuad(-1.f, 0.f, 0.25f, nearColor);
    auto farQuad = makeFullQuad(farColor, 0.75f);

    runFrame(
        [&](Frame& frame)
        {
            clearSlices(frame, cube);

            const ColorTarget targets[] = {sliceOf(cube, 1, false),
                                           sliceOf(cube, 4, false)};
            auto pass = frame.beginMultiTargetPass(targets);
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(nearQuad);
            pass.draw(6);
            pass.setVertexBuffer(farQuad);
            pass.draw(6);
        });

    const auto row = targetSize * (targetSize / 2);
    const auto faceOne = readSlice(cube, 1);
    const auto faceFour = readSlice(cube, 4);

    check(matches(faceOne[row], 255, 0, 0));
    check(matches(faceOne[row + targetSize - 1], 0, 255, 0));
    check(matches(faceFour[row], 0, 0, 255));
    check(matches(faceFour[row + targetSize - 1], 0, 255, 0));

    for (auto face: {0, 2, 3, 5})
        check(matches(readSlice(cube, face)[0], 128, 128, 128));
};
