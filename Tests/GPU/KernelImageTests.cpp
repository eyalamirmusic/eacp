#include "Common.h"

#include <eacp/GPU/Codegen/PackedScalars.h>

#include <cmath>
#include <cstring>
#include <functional>

// What a kernel reads and writes beyond RGBA: every format a kernel can store
// to on the device (Device::supportsComputeWrite), a texel read and written
// back through one bind where the device takes that
// (Device::supportsComputeReadWrite), a render target's depth plane read by a
// kernel (ComputePass::setInputDepthTexture) whole or one slice at a time as a
// depth2d (the ...DepthTextureSlice binds), a pass taking its depth plane from
// another target (RenderPassDescriptor::depthTarget), and a cube written and
// read as the 2D array of its faces (ComputePass::setOutputTextureAsArray and
// setInputTextureAsArray) and then sampled as a cube by a draw. The shaders
// are hand-written, the way bgfx's are.
//
// Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace KernelImageTests
{
namespace
{
constexpr auto imageSize = 4;
constexpr auto texelCount = imageSize * imageSize;

bool canTest()
{
    return Device::shared().isValid();
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

void runFrame(std::function<void(Frame&)> draw)
{
    auto view = FrameRunner {};
    view.draw = std::move(draw);
    view.setBounds({0.f, 0.f, 4.f, 4.f});
    view.renderToImage(1.f);
}

ComputePipeline makeKernel(
    ShaderTwins twins,
    const std::source_location& location = std::source_location::current())
{
    auto source = nativeComputeShaderSource(std::move(twins), location)
                      .withCompute("computeMain");
    auto library = Device::shared().makeShaderLibrary(source);
    return Device::shared().makeComputePipeline(library);
}

ComputePipeline makeKernel(
    std::string_view name,
    const std::source_location& location = std::source_location::current())
{
    return makeKernel(shaderTwins(name, location), location);
}

bool near(float value, float expected, float tolerance)
{
    return std::fabs(value - expected) <= tolerance;
}

// One format a kernel writes, and how a test reads a channel of it back.
struct WrittenFormat
{
    TextureFormat format;
    const char* glslQualifier;
    int channels;
    const char* wgslFormat;
};

constexpr WrittenFormat floatFormats[] = {
    {TextureFormat::R8Unorm, "r8", 1, "r8unorm"},
    {TextureFormat::RG8Unorm, "rg8", 2, "rg8unorm"},
    {TextureFormat::R16Float, "r16f", 1, "r16float"},
    {TextureFormat::RG16Float, "rg16f", 2, "rg16float"},
    {TextureFormat::R32Float, "r32f", 1, "r32float"},
    {TextureFormat::RG32Float, "rg32f", 2, "rg32float"},
    {TextureFormat::R16Unorm, "r16", 1, "r16unorm"},
    {TextureFormat::RGBA8Unorm, "rgba8", 4, "rgba8unorm"},
    {TextureFormat::RGBA16Float, "rgba16f", 4, "rgba16float"},
    {TextureFormat::RGBA32Float, "rgba32f", 4, "rgba32float"},
};

float channelOf(TextureFormat format, const std::uint8_t* texel, int channel)
{
    switch (format)
    {
        case TextureFormat::R8Unorm:
        case TextureFormat::RG8Unorm:
        case TextureFormat::RGBA8Unorm:
            return (float) texel[channel] / 255.f;

        case TextureFormat::R16Float:
        case TextureFormat::RG16Float:
        case TextureFormat::RGBA16Float:
        {
            auto bits = std::uint16_t {};
            std::memcpy(&bits, texel + channel * 2, sizeof(bits));
            return halfToFloat(bits);
        }

        case TextureFormat::R16Unorm:
        {
            auto bits = std::uint16_t {};
            std::memcpy(&bits, texel + channel * 2, sizeof(bits));
            return (float) bits / 65535.f;
        }

        default:
        {
            auto value = 0.f;
            std::memcpy(&value, texel + channel * 4, sizeof(value));
            return value;
        }
    }
}

// Texel (x, y) gets a = (x + 4y + 1) / 255 and b = (3x + 2) / 255: a value of
// its own in every texel, and one an 8-bit channel holds exactly.
std::string glslWriteFloats(const char* qualifier)
{
    return R"(#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 8, )"
           + std::string {qualifier} + R"() uniform writeonly image2D target;

void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(target);

    if (gid.x >= size.x || gid.y >= size.y)
        return;

    float a = float(gid.x + gid.y * 4 + 1) / 255.0;
    float b = float(gid.x * 3 + 2) / 255.0;
    imageStore(target, gid, vec4(a, b, 0.5, 1.0));
}
)";
}

std::string wgslWriteFloats(const char* format)
{
    return R"(
@group(0) @binding(17) var image: texture_storage_2d<)"
           + std::string {format} + R"(, write>;

@compute @workgroup_size(8, 8)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    let size = textureDimensions(image);

    if (gid.x >= size.x || gid.y >= size.y)
    {
        return;
    }

    let a = f32(gid.x + gid.y * 4u + 1u) / 255.0;
    let b = f32(gid.x * 3u + 2u) / 255.0;
    textureStore(image, gid.xy, vec4f(a, b, 0.5, 1.0));
}
)";
}

Texture makeImage(TextureFormat format)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = imageSize;
    descriptor.height = imageSize;
    descriptor.format = format;
    descriptor.computeWrite = true;
    return Device::shared().makeTexture(descriptor);
}

void dispatchInto(const ComputePipeline& pipeline, const Texture& target)
{
    runFrame(
        [&](Frame& frame)
        {
            auto pass = frame.beginCompute();
            pass.setPipeline(pipeline);
            pass.setOutputTexture(target, 0);
            pass.dispatch(imageSize, imageSize);
        });
}

Vector<std::uint8_t> readTexels(const Texture& texture)
{
    auto bytes = Vector<std::uint8_t> {};
    bytes.resize((std::size_t) (texelCount * bytesPerPixel(texture.format())));
    texture.read(bytes.data());
    return bytes;
}

bool holdsTheWrittenValues(const WrittenFormat& written, const Texture& texture)
{
    const auto bytes = readTexels(texture);
    const auto stride = bytesPerPixel(written.format);

    for (auto y = 0; y < imageSize; ++y)
        for (auto x = 0; x < imageSize; ++x)
        {
            const auto* texel = bytes.data() + (y * imageSize + x) * stride;
            const auto a = (float) (x + y * 4 + 1) / 255.f;
            const auto b = (float) (x * 3 + 2) / 255.f;

            if (!near(channelOf(written.format, texel, 0), a, 1.5e-3f))
                return false;

            if (written.channels > 1
                && !near(channelOf(written.format, texel, 1), b, 1.5e-3f))
                return false;
        }

    return true;
}

// Each texel seeded with s = (x + 4y + 1) / 32 - eight bits' worth, and
// exact in a half - and rewritten as 1 - s through the one bind.
constexpr WrittenFormat readWriteFormats[] = {
    {TextureFormat::R32Float, "r32f", 1, "r32float"},
    {TextureFormat::R16Float, "r16f", 1, "r16float"},
    {TextureFormat::R8Unorm, "r8", 1, "r8unorm"},
    {TextureFormat::RGBA8Unorm, "rgba8", 4, "rgba8unorm"},
    {TextureFormat::RGBA16Float, "rgba16f", 4, "rgba16float"},
};

std::string glslReadWriteFloats(const char* qualifier)
{
    return R"(#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 8, )"
           + std::string {qualifier} + R"() uniform image2D image;

void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(image);

    if (gid.x >= size.x || gid.y >= size.y)
        return;

    vec4 value = imageLoad(image, gid);
    imageStore(image, gid, vec4(1.0 - value.x, value.yzw));
}
)";
}

std::string wgslReadWriteFloats(const char* format)
{
    return R"(
@group(0) @binding(17) var image: texture_storage_2d<)"
           + std::string {format} + R"(, read_write>;

@compute @workgroup_size(8, 8)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    let size = textureDimensions(image);

    if (gid.x >= size.x || gid.y >= size.y)
    {
        return;
    }

    let value = textureLoad(image, gid.xy);
    textureStore(image, gid.xy, vec4f(1.0 - value.x, value.yzw));
}
)";
}

float seedAt(int x, int y)
{
    return (float) (x + y * 4 + 1) / 32.f;
}

// Channel 0 holds the seed; the rest hold zero, which the kernel keeps.
Vector<std::uint8_t> seedTexels(TextureFormat format)
{
    const auto stride = bytesPerPixel(format);
    auto bytes = Vector<std::uint8_t> {};
    bytes.resize((std::size_t) (texelCount * stride));

    for (auto y = 0; y < imageSize; ++y)
        for (auto x = 0; x < imageSize; ++x)
        {
            auto* texel = bytes.data() + (y * imageSize + x) * stride;
            const auto seed = seedAt(x, y);

            if (format == TextureFormat::R8Unorm
                || format == TextureFormat::RGBA8Unorm)
            {
                texel[0] = (std::uint8_t) std::lround(seed * 255.f);
            }
            else if (format == TextureFormat::R16Float
                     || format == TextureFormat::RGBA16Float)
            {
                const auto bits = halfFromFloat(seed);
                std::memcpy(texel, &bits, sizeof(bits));
            }
            else
            {
                std::memcpy(texel, &seed, sizeof(seed));
            }
        }

    return bytes;
}

// The depth target and the quads drawn into it: hand-written, one constant
// colour, the vertex's z as it stands.
struct DepthVertex
{
    float position[3];
};

RenderPipeline makeDepthPipeline()
{
    auto source = nativeShaderSource("KernelImage/depthQuad");
    auto library = Device::shared().makeShaderLibrary(
        source.withVertex("vertexMain").withFragment("fragmentMain"));

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.depth = true;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
    descriptor.vertexLayout.attribute(VertexFormat::Float3, 0);
    descriptor.vertexLayout.stride = sizeof(DepthVertex);

    return Device::shared().makeRenderPipeline(descriptor);
}

// A quad from x = -1 to `right` at depth z.
Buffer makeDepthQuad(float right, float z)
{
    const DepthVertex vertices[] = {{{-1.f, -1.f, z}},
                                    {{right, -1.f, z}},
                                    {{-1.f, 1.f, z}},
                                    {{right, -1.f, z}},
                                    {{right, 1.f, z}},
                                    {{-1.f, 1.f, z}}};

    return Device::shared().makeBuffer(vertices, sizeof(vertices));
}

TextureDescriptor describeDepthScene(TextureType kind, int layers)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = imageSize;
    descriptor.height = imageSize;
    descriptor.kind = kind;
    descriptor.arrayLength = layers;
    descriptor.renderTarget = true;
    descriptor.sampleableDepth = true;
    return descriptor;
}

// Depth into one slice of the scene, kept for the kernel.
void drawDepth(Frame& frame,
               const RenderPipeline& pipeline,
               const Texture& scene,
               int slice,
               const Buffer& quad)
{
    auto target = ColorTarget {&scene};
    target.slice = slice;

    auto descriptor = RenderPassDescriptor {};
    descriptor.depthAction = DepthAction::Keep;

    auto pass = frame.beginMultiTargetPass(std::span<const ColorTarget> {&target, 1},
                                           descriptor);
    pass.setPipeline(pipeline);
    pass.setVertexBuffer(quad);
    pass.draw(6);
}

Vector<float> readFloats(const Buffer& buffer, int count)
{
    auto values = Vector<float> {};
    values.resize((std::size_t) count);
    buffer.read(values.data(), (std::int64_t) (count * (int) sizeof(float)));
    return values;
}

// The left half at `left`, the right half at the far plane the pass cleared to.
bool holdsHalfAndHalf(const Vector<float>& values, float left)
{
    if ((int) values.size() != texelCount)
        return false;

    for (auto y = 0; y < imageSize; ++y)
        for (auto x = 0; x < imageSize; ++x)
        {
            const auto expected = x < imageSize / 2 ? left : 1.f;

            if (!near(values[(std::size_t) (y * imageSize + x)], expected, 1.0e-5f))
                return false;
        }

    return true;
}

// A 6 x 1 target, column n sampling the cube along face n's axis.
struct PositionVertex
{
    float position[2];
};

constexpr PositionVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

std::uint32_t faceColor(int face)
{
    return 0xff000000u | ((std::uint32_t) 64 << 16)
           | ((std::uint32_t) (200 - 30 * face) << 8)
           | (std::uint32_t) (40 * face + 20);
}

bool isNear(std::uint32_t a, std::uint32_t b)
{
    for (auto shift = 0; shift < 32; shift += 8)
    {
        const auto x = (int) ((a >> shift) & 0xffu);
        const auto y = (int) ((b >> shift) & 0xffu);

        if (x < y - 2 || x > y + 2)
            return false;
    }

    return true;
}

std::uint32_t packColor(const float* rgba)
{
    auto packed = std::uint32_t {0};

    for (auto channel = 0; channel < 4; ++channel)
        packed |= (std::uint32_t) std::lround(rgba[channel] * 255.f)
                  << (8 * channel);

    return packed;
}
} // namespace

auto tKernelImageDeviceAnswers =
    test("KernelImage/theDeviceSaysWhichFormatsAKernelWrites") = []
{
    if (!canTest())
        return;

    auto& device = Device::shared();

    for (auto format: {TextureFormat::RGBA8Unorm,
                       TextureFormat::RGBA16Float,
                       TextureFormat::RGBA32Float})
    {
        check(supportsComputeWrite(format));
        check(device.supportsComputeWrite(format));
    }

    check(!device.supportsComputeWrite(TextureFormat::BGRA8Unorm));
    check(!device.supportsComputeWrite(TextureFormat::BC1RGBA));

    // Every device eacp runs on reads and writes a 32-bit channel through one
    // bind: Metal's first tier, D3D12's guaranteed typed loads, a storage
    // image on Vulkan.
    check(device.supportsComputeReadWrite(TextureFormat::R32Float));
    check(device.supportsComputeReadWrite(TextureFormat::R32Uint));

    for (const auto& written: floatFormats)
        if (device.supportsComputeReadWrite(written.format))
            check(device.supportsComputeWrite(written.format));

    if constexpr (Platform::isApple())
    {
        for (const auto& written: floatFormats)
            check(device.supportsComputeWrite(written.format));

        check(device.supportsComputeWrite(TextureFormat::R32Uint));
        check(!device.supportsComputeReadWrite(TextureFormat::RG16Float));
    }

    // A format the device refuses is an invalid kernel output, not an
    // unwritable one.
    check(!makeImage(TextureFormat::BGRA8Unorm).isValid());
};

auto tKernelImageWritesEachFormat =
    test("KernelImage/aKernelWritesEveryFormatTheDeviceTakes") = []
{
    if (!canTest())
        return;

    for (const auto& written: floatFormats)
    {
        const auto writable = Device::shared().supportsComputeWrite(written.format);
        auto texture = makeImage(written.format);

        check(texture.isValid() == writable);

        if (!writable)
            continue;

        auto twins = shaderTwins("KernelImage/writeFloats");
        twins.glsl = glslWriteFloats(written.glslQualifier);
        twins.wgsl = wgslWriteFloats(written.wgslFormat);
        auto pipeline = makeKernel(std::move(twins));

        check(pipeline.isValid());

        dispatchInto(pipeline, texture);

        check(holdsTheWrittenValues(written, texture));
    }
};

auto tKernelImageWritesIntegers =
    test("KernelImage/aKernelWritesAnIntegerImage") = []
{
    if (!canTest() || !Device::shared().supportsComputeWrite(TextureFormat::R32Uint))
        return;

    auto texture = makeImage(TextureFormat::R32Uint);
    auto pipeline = makeKernel("KernelImage/writeUints");

    check(texture.isValid() && pipeline.isValid());

    dispatchInto(pipeline, texture);

    auto values = Vector<std::uint32_t> {};
    values.resize(texelCount);
    texture.read(values.data());

    for (auto index = 0; index < texelCount; ++index)
        check(values[(std::size_t) index] == (std::uint32_t) index * 1000003u + 7u);
};

auto tKernelImageReadWrite =
    test("KernelImage/aKernelReadsAndWritesATexelThroughOneBind") = []
{
    if (!canTest())
        return;

    for (const auto& written: readWriteFormats)
    {
        if (!Device::shared().supportsComputeReadWrite(written.format))
            continue;

        auto texture = makeImage(written.format);
        auto twins = shaderTwins("KernelImage/readWriteFloats");
        twins.glsl = glslReadWriteFloats(written.glslQualifier);
        twins.wgsl = wgslReadWriteFloats(written.wgslFormat);
        auto pipeline = makeKernel(std::move(twins));

        check(texture.isValid() && pipeline.isValid());

        const auto seed = seedTexels(written.format);
        texture.update(seed.data());

        dispatchInto(pipeline, texture);

        const auto bytes = readTexels(texture);
        const auto stride = bytesPerPixel(written.format);

        for (auto y = 0; y < imageSize; ++y)
            for (auto x = 0; x < imageSize; ++x)
            {
                const auto* texel = bytes.data() + (y * imageSize + x) * stride;
                check(near(channelOf(written.format, texel, 0),
                           1.f - seedAt(x, y),
                           2.5e-3f));
            }
    }

    auto integers = makeImage(TextureFormat::R32Uint);
    auto pipeline = makeKernel("KernelImage/readWriteUints");

    check(integers.isValid() && pipeline.isValid());

    auto seed = Vector<std::uint32_t> {};

    for (auto index = 0; index < texelCount; ++index)
        seed.add((std::uint32_t) index * 11u + 5u);

    integers.update(seed.data());
    dispatchInto(pipeline, integers);

    auto values = Vector<std::uint32_t> {};
    values.resize(texelCount);
    integers.read(values.data());

    for (auto index = 0; index < texelCount; ++index)
        check(values[(std::size_t) index] == seed[(std::size_t) index] * 3u + 1u);
};

auto tKernelImageReadsDepth = test("KernelImage/aKernelReadsATargetsDepth") = []
{
    if (!canTest())
        return;

    auto scene =
        Device::shared().makeTexture(describeDepthScene(TextureType::TwoD, 1));
    auto pipeline = makeDepthPipeline();
    auto kernel = makeKernel("KernelImage/copyDepth");
    auto quad = makeDepthQuad(0.f, 0.25f);
    auto values = Device::shared().makeBuffer(texelCount * (int) sizeof(float));

    check(scene.isValid() && scene.hasSampleableDepth());
    check(pipeline.isValid() && kernel.isValid() && values.isValid());

    runFrame(
        [&](Frame& frame)
        {
            drawDepth(frame, pipeline, scene, 0, quad);

            auto pass = frame.beginCompute();
            pass.setPipeline(kernel);
            pass.setInputDepthTexture(scene, 0);
            pass.setOutputBuffer(values, 0);
            pass.dispatch(imageSize, imageSize);
        });

    check(holdsHalfAndHalf(readFloats(values, texelCount), 0.25f));
};

// Every backend reads an array target's depth as the array, level 0 of each
// layer.
auto tKernelImageReadsDepthLayer =
    test("KernelImage/aKernelReadsOneLayerOfAnArraysDepth") = []
{
    if (!canTest())
        return;

    auto scene =
        Device::shared().makeTexture(describeDepthScene(TextureType::TwoDArray, 2));
    auto pipeline = makeDepthPipeline();
    auto kernel = makeKernel("KernelImage/copyDepthLayer");
    auto farQuad = makeDepthQuad(0.f, 0.75f);
    auto nearQuad = makeDepthQuad(0.f, 0.25f);
    auto layerZero = Device::shared().makeBuffer(texelCount * (int) sizeof(float));
    auto layerOne = Device::shared().makeBuffer(texelCount * (int) sizeof(float));

    check(scene.isValid() && scene.hasSampleableDepth());
    check(pipeline.isValid() && kernel.isValid());

    runFrame(
        [&](Frame& frame)
        {
            drawDepth(frame, pipeline, scene, 0, farQuad);
            drawDepth(frame, pipeline, scene, 1, nearQuad);

            auto pass = frame.beginCompute();
            pass.setPipeline(kernel);
            pass.setInputDepthTexture(scene, 0);

            for (auto layer: {0, 1})
            {
                const auto index = (std::uint32_t) layer;
                pass.setOutputBuffer(layer == 0 ? layerZero : layerOne, 0);
                pass.setUniform(index);
                pass.dispatch(imageSize, imageSize);
            }
        });

    check(holdsHalfAndHalf(readFloats(layerZero, texelCount), 0.75f));
    check(holdsHalfAndHalf(readFloats(layerOne, texelCount), 0.25f));
};

auto tKernelImageCubeAsArray =
    test("KernelImage/aKernelWritesACubeAsAnArrayAndADrawSamplesIt") = []
{
    if (!canTest())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = imageSize;
    descriptor.height = imageSize;
    descriptor.kind = TextureType::Cube;
    descriptor.computeWrite = true;

    auto cube = Device::shared().makeTexture(descriptor);

    auto targetDescriptor = TextureDescriptor {};
    targetDescriptor.width = 6;
    targetDescriptor.height = 1;
    targetDescriptor.renderTarget = true;

    auto columns = Device::shared().makeTexture(targetDescriptor);

    // Face n gets its own colour, (40n + 20, 200 - 30n, 64, 255) in bytes, and
    // texel (1, 1) of every face is read back through the array view.
    auto writeFaces = makeKernel("KernelImage/writeFaces");
    auto readFaces = makeKernel("KernelImage/readFaces");
    auto faceValues = Device::shared().makeBuffer(6 * 4 * (int) sizeof(float));

    auto source = nativeShaderSource("KernelImage/sampleCube");
    auto library = Device::shared().makeShaderLibrary(
        source.withVertex("vertexMain").withFragment("fragmentMain"));

    auto pipelineDescriptor = RenderPipelineDescriptor {};
    pipelineDescriptor.library = &library;
    pipelineDescriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
    pipelineDescriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
    pipelineDescriptor.vertexLayout.stride = sizeof(PositionVertex);

    auto sampleCube = Device::shared().makeRenderPipeline(pipelineDescriptor);
    auto quad = Device::shared().makeBuffer(fullQuad, sizeof(fullQuad));

    check(cube.isValid() && cube.isCube() && columns.isValid());
    check(writeFaces.isValid() && readFaces.isValid() && sampleCube.isValid());

    runFrame(
        [&](Frame& frame)
        {
            {
                auto pass = frame.beginCompute();
                pass.setPipeline(writeFaces);
                pass.setOutputTextureAsArray(cube, 0);
                pass.dispatch(imageSize, imageSize, 6);
            }

            {
                auto pass = frame.beginCompute();
                pass.setPipeline(readFaces);
                pass.setInputTextureAsArray(cube, 0);
                pass.setOutputBuffer(faceValues, 0);
                pass.dispatch(6);
            }

            auto pass = frame.beginPass(columns, {Graphics::Color::black()});
            pass.setPipeline(sampleCube);
            pass.setVertexBuffer(quad);
            pass.setFragmentTexture(cube, 0);
            pass.draw(6);
        });

    auto sampled = Array<std::uint32_t, 6> {};
    columns.read(sampled.data());

    const auto read = readFloats(faceValues, 6 * 4);

    for (auto face = 0; face < 6; ++face)
    {
        auto pixels = Vector<std::uint32_t> {};
        pixels.resize(texelCount);
        cube.readSubresource(TextureRegion::wholeLevel(0, face), pixels.data());

        for (auto pixel: pixels)
            check(isNear(pixel, faceColor(face)));

        check(isNear(sampled[(std::size_t) face], faceColor(face)));

        if ((int) read.size() == 6 * 4)
            check(isNear(packColor(read.data() + face * 4), faceColor(face)));
    }
};

namespace
{
// The 2D kernel above, with D3D12's own: its view of one slice is a one-slice
// Texture2DArray rather than a Texture2D.
ComputePipeline makeSliceKernel()
{
    auto twins = shaderTwins("KernelImage/copyDepth");
    twins.hlsl = testShaderFile("KernelImage/copyDepthSlice.hlsl");
    return makeKernel(std::move(twins));
}

// Every slice of `scene` has the far plane but `nearSlice`, whose left half
// is at 0.25; a kernel then copies each slice through its own 2D bind.
bool eachSliceReadsAs2D(TextureType kind, int layers, int nearSlice)
{
    auto scene = Device::shared().makeTexture(describeDepthScene(kind, layers));
    auto pipeline = makeDepthPipeline();
    auto kernel = makeSliceKernel();
    auto nearQuad = makeDepthQuad(0.f, 0.25f);
    auto nothing = makeDepthQuad(-1.f, 0.25f);

    check(scene.isValid() && scene.hasSampleableDepth());
    check(pipeline.isValid() && kernel.isValid());

    if (!scene.isValid())
        return false;

    auto slices = Vector<Buffer> {};

    for (auto slice = 0; slice < scene.slices(); ++slice)
        slices.add(Device::shared().makeBuffer(texelCount * (int) sizeof(float)));

    runFrame(
        [&](Frame& frame)
        {
            for (auto slice = 0; slice < scene.slices(); ++slice)
                drawDepth(frame,
                          pipeline,
                          scene,
                          slice,
                          slice == nearSlice ? nearQuad : nothing);

            auto pass = frame.beginCompute();
            pass.setPipeline(kernel);

            for (auto slice = 0; slice < scene.slices(); ++slice)
            {
                pass.setInputDepthTextureSlice(scene, slice, 0);
                pass.setOutputBuffer(slices[(std::size_t) slice], 0);
                pass.dispatch(imageSize, imageSize);
            }
        });

    auto all = true;

    for (auto slice = 0; slice < scene.slices(); ++slice)
    {
        const auto values = readFloats(slices[(std::size_t) slice], texelCount);
        all = all && holdsHalfAndHalf(values, slice == nearSlice ? 0.25f : 1.f);
    }

    return all;
}
} // namespace

auto tKernelImageDepthSliceOfArray =
    test("KernelImage/aKernelReadsOneLayerOfAnArraysDepthAs2D") = []
{
    if (!canTest())
        return;

    check(eachSliceReadsAs2D(TextureType::TwoDArray, 3, 1));
};

auto tKernelImageDepthSliceOfCube =
    test("KernelImage/aKernelReadsOneFaceOfACubesDepthAs2D") = []
{
    if (!canTest())
        return;

    check(eachSliceReadsAs2D(TextureType::Cube, 1, 4));
};

// The fragment stage's slice bind: layer 1 of an array's depth drawn into an
// R32Float target through a depth2d declaration.
auto tKernelImageFragmentDepthSlice =
    test("KernelImage/aFragmentShaderReadsOneLayerOfAnArraysDepthAs2D") = []
{
    if (!canTest())
        return;

    auto scene =
        Device::shared().makeTexture(describeDepthScene(TextureType::TwoDArray, 2));
    auto depthPipeline = makeDepthPipeline();
    auto nearQuad = makeDepthQuad(0.f, 0.25f);
    auto farQuad = makeDepthQuad(0.f, 0.75f);

    auto targetDescriptor = TextureDescriptor {};
    targetDescriptor.width = imageSize;
    targetDescriptor.height = imageSize;
    targetDescriptor.format = TextureFormat::R32Float;
    targetDescriptor.renderTarget = true;

    auto copy = Device::shared().makeTexture(targetDescriptor);

    // A full-screen quad whose fragment shader writes the depth2d at its pixel.
    auto source = nativeShaderSource("KernelImage/depthToColor");
    auto library = Device::shared().makeShaderLibrary(
        source.withVertex("vertexMain").withFragment("fragmentMain"));

    auto pipelineDescriptor = RenderPipelineDescriptor {};
    pipelineDescriptor.library = &library;
    pipelineDescriptor.colorFormat = pixelFormatFor(TextureFormat::R32Float);
    pipelineDescriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
    pipelineDescriptor.vertexLayout.stride = sizeof(PositionVertex);

    auto readDepth = Device::shared().makeRenderPipeline(pipelineDescriptor);
    auto quad = Device::shared().makeBuffer(fullQuad, sizeof(fullQuad));

    check(scene.isValid() && copy.isValid());
    check(depthPipeline.isValid() && readDepth.isValid());

    if (!scene.isValid() || !copy.isValid())
        return;

    runFrame(
        [&](Frame& frame)
        {
            drawDepth(frame, depthPipeline, scene, 0, farQuad);
            drawDepth(frame, depthPipeline, scene, 1, nearQuad);

            auto pass = frame.beginPass(copy);
            pass.setPipeline(readDepth);
            pass.setFragmentDepthTextureSlice(scene, 1, 0);
            pass.setVertexBuffer(quad);
            pass.draw(6);
        });

    auto values = Vector<float> {};
    values.resize((std::size_t) texelCount);
    copy.read(values.data());

    check(holdsHalfAndHalf(values, 0.25f));
};

namespace
{
TextureDescriptor describeColorOnly(TextureType kind, int layers, int depth = 1)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = imageSize;
    descriptor.height = imageSize;
    descriptor.kind = kind;
    descriptor.arrayLength = layers;
    descriptor.volumeDepth = depth;
    descriptor.renderTarget = true;
    return descriptor;
}

// One image of `colour` with its depth plane from `depth` at `depthSlice`,
// the near quad drawn over the left half and kept.
bool drawWithDepthFrom(Frame& frame,
                       const RenderPipeline& pipeline,
                       const Texture& colour,
                       int slice,
                       const Texture& depth,
                       int depthSlice,
                       const Buffer& quad)
{
    auto target = ColorTarget {&colour};
    target.slice = slice;

    auto descriptor = RenderPassDescriptor {};
    descriptor.depthAction = DepthAction::Keep;
    descriptor.depthTarget = {&depth, 0, depthSlice};

    auto pass = frame.beginMultiTargetPass(std::span<const ColorTarget> {&target, 1},
                                           descriptor);

    if (pass.targetWidth() == 0)
        return false;

    pass.setPipeline(pipeline);
    pass.setVertexBuffer(quad);
    pass.draw(6);
    return true;
}

Vector<std::uint32_t> readColorImage(const Texture& texture, int slice)
{
    const auto volume = texture.kind() == TextureType::ThreeD;
    auto region = TextureRegion::wholeLevel(0, volume ? 0 : slice);

    if (volume)
    {
        region.origin.z = slice;
        region.size.depth = 1;
    }

    auto values = Vector<std::uint32_t> {};
    values.resize((std::size_t) texelCount);
    texture.readSubresource(region, values.data());
    return values;
}

// The left half red, which is what the depth pipeline draws, and the right
// half the black the pass cleared to.
bool holdsRedLeftHalf(const Vector<std::uint32_t>& values)
{
    if ((int) values.size() != texelCount)
        return false;

    for (auto y = 0; y < imageSize; ++y)
        for (auto x = 0; x < imageSize; ++x)
        {
            const auto expected = x < imageSize / 2 ? 0xff0000ffu : 0xff000000u;

            if (values[(std::size_t) (y * imageSize + x)] != expected)
                return false;
        }

    return true;
}
} // namespace

// Layer 1 of a colour-only array, and one image of a volume, each tested
// against a plain 2D depth target named by RenderPassDescriptor::depthTarget;
// the 2D target's plane holds what was drawn and samples as depth2d.
auto tKernelImageSeparateDepthTarget =
    test("KernelImage/aPassTakesItsDepthFromAnother2DTarget") = []
{
    if (!canTest())
        return;

    auto array =
        Device::shared().makeTexture(describeColorOnly(TextureType::TwoDArray, 2));
    auto volume =
        Device::shared().makeTexture(describeColorOnly(TextureType::ThreeD, 1, 4));
    auto arrayDepth =
        Device::shared().makeTexture(describeDepthScene(TextureType::TwoD, 1));
    auto volumeDepth =
        Device::shared().makeTexture(describeDepthScene(TextureType::TwoD, 1));
    auto pipeline = makeDepthPipeline();
    auto kernel = makeKernel("KernelImage/copyDepth");
    auto nearQuad = makeDepthQuad(0.f, 0.25f);
    auto arrayValues = Device::shared().makeBuffer(texelCount * (int) sizeof(float));
    auto volumeValues =
        Device::shared().makeBuffer(texelCount * (int) sizeof(float));

    check(array.isValid() && volume.isValid());
    check(arrayDepth.hasSampleableDepth() && volumeDepth.hasSampleableDepth());
    check(pipeline.isValid() && kernel.isValid());

    if (!array.isValid() || !volume.isValid())
        return;

    auto drawn = Vector<int> {};

    runFrame(
        [&](Frame& frame)
        {
            drawn.add(drawWithDepthFrom(
                frame, pipeline, array, 1, arrayDepth, 0, nearQuad));
            drawn.add(drawWithDepthFrom(
                frame, pipeline, volume, 2, volumeDepth, 0, nearQuad));

            auto pass = frame.beginCompute();
            pass.setPipeline(kernel);
            pass.setInputDepthTexture(arrayDepth, 0);
            pass.setOutputBuffer(arrayValues, 0);
            pass.dispatch(imageSize, imageSize);
            pass.setInputDepthTexture(volumeDepth, 0);
            pass.setOutputBuffer(volumeValues, 0);
            pass.dispatch(imageSize, imageSize);
        });

    check(drawn.size() == 2 && drawn[0] && drawn[1]);
    check(holdsRedLeftHalf(readColorImage(array, 1)));
    check(holdsRedLeftHalf(readColorImage(volume, 2)));
    check(holdsHalfAndHalf(readFloats(arrayValues, texelCount), 0.25f));
    check(holdsHalfAndHalf(readFloats(volumeValues, texelCount), 0.25f));
};

// The depth plane of an array target at another slice than its colour's
// (layer 0's colour, layer 1's depth), and the refusals: a depth target of
// another size, one with no depth, and a slice the plane lacks.
auto tKernelImageDepthAtAnotherSlice =
    test("KernelImage/aPassTakesItsDepthAtAnotherSlice") = []
{
    if (!canTest())
        return;

    auto scene =
        Device::shared().makeTexture(describeDepthScene(TextureType::TwoDArray, 2));
    auto colourOnly =
        Device::shared().makeTexture(describeColorOnly(TextureType::TwoD, 1));

    auto largerDescriptor = describeDepthScene(TextureType::TwoD, 1);
    largerDescriptor.width = imageSize * 2;
    largerDescriptor.height = imageSize * 2;
    auto larger = Device::shared().makeTexture(largerDescriptor);

    auto pipeline = makeDepthPipeline();
    auto kernel = makeSliceKernel();
    auto nearQuad = makeDepthQuad(0.f, 0.25f);
    auto nothing = makeDepthQuad(-1.f, 0.25f);
    auto layerZero = Device::shared().makeBuffer(texelCount * (int) sizeof(float));
    auto layerOne = Device::shared().makeBuffer(texelCount * (int) sizeof(float));

    check(scene.isValid() && colourOnly.isValid() && larger.isValid());
    check(pipeline.isValid() && kernel.isValid());

    if (!scene.isValid())
        return;

    auto drawn = Vector<int> {};

    runFrame(
        [&](Frame& frame)
        {
            drawDepth(frame, pipeline, scene, 0, nothing);
            drawDepth(frame, pipeline, scene, 1, nothing);

            drawn.add(
                drawWithDepthFrom(frame, pipeline, scene, 0, scene, 1, nearQuad));
            drawn.add(
                drawWithDepthFrom(frame, pipeline, scene, 0, larger, 0, nearQuad));
            drawn.add(drawWithDepthFrom(
                frame, pipeline, scene, 0, colourOnly, 0, nearQuad));
            drawn.add(
                drawWithDepthFrom(frame, pipeline, scene, 0, scene, 2, nearQuad));

            auto pass = frame.beginCompute();
            pass.setPipeline(kernel);
            pass.setInputDepthTextureSlice(scene, 0, 0);
            pass.setOutputBuffer(layerZero, 0);
            pass.dispatch(imageSize, imageSize);
            pass.setInputDepthTextureSlice(scene, 1, 0);
            pass.setOutputBuffer(layerOne, 0);
            pass.dispatch(imageSize, imageSize);
        });

    check(drawn.size() == 4);

    if (drawn.size() == 4)
    {
        check(drawn[0]);
        check(!drawn[1] && !drawn[2] && !drawn[3]);
    }

    check(holdsRedLeftHalf(readColorImage(scene, 0)));
    check(holdsHalfAndHalf(readFloats(layerZero, texelCount), 1.f));
    check(holdsHalfAndHalf(readFloats(layerOne, texelCount), 0.25f));
};
} // namespace KernelImageTests
