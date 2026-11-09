#include "Common.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

// WGSL's texture functions on the device. Every level of every slice is one
// solid colour whose red says the level and whose green says the slice (or a
// volume's image), so a read of the wrong level, layer, face or image comes
// back as a number no right answer has. The queries, the level, gradient,
// gather and load reads run in a kernel, which is where WGSL allows all of
// them; the bias and the implicit samples of the new kinds run in a fragment
// shader; the depth kinds read the planes of targets drawn in the same frame.
//
// Self-skips without a device, and the cube-array cases without cube arrays.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace TextureFunctionDeviceTests
{
namespace
{
struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace TextureFunctionDeviceTests

EACP_SHADER_VALUE(TextureFunctionDeviceTests::QuadVertex, Float2)

namespace TextureFunctionDeviceTests
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

bool canTest()
{
    return Device::shared().isValid();
}

bool canTestCubeArrays()
{
    return canTest() && Device::shared().supportsCubeArrays();
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

int levelRed(int level)
{
    return 40 * (level + 1);
}

int sliceGreen(int slice)
{
    return 10 * (slice + 1);
}

std::uint32_t levelColour(int slice, int level)
{
    return rgba(levelRed(level), sliceGreen(slice), 0);
}

float unorm(int value)
{
    return (float) value / 255.f;
}

int extent(int size, int level)
{
    return std::max(1, size >> level);
}

int slicesOf(TextureType kind, int layers)
{
    if (kind == TextureType::Cube)
        return 6;

    if (kind == TextureType::CubeArray)
        return 6 * layers;

    return kind == TextureType::TwoDArray ? layers : 1;
}

// Every level of every slice solid, slice-major; a volume level-major, each of
// its images solid in its own colour.
Texture makeLevelled(TextureType kind,
                     int width,
                     int height,
                     int levels,
                     int layers = 1,
                     int depth = 1)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.kind = kind;
    descriptor.arrayLength = layers;
    descriptor.volumeDepth = depth;
    descriptor.mipLevels = levels;

    auto pixels = Vector<std::uint32_t> {};
    auto addLevel = [&](int slice, int level)
    {
        for (auto i = 0; i < extent(width, level) * extent(height, level); ++i)
            pixels.add(levelColour(slice, level));
    };

    if (kind == TextureType::ThreeD)
    {
        for (auto level = 0; level < levels; ++level)
            for (auto image = 0; image < extent(depth, level); ++image)
                addLevel(image, level);
    }
    else
    {
        for (auto slice = 0; slice < slicesOf(kind, layers); ++slice)
            for (auto level = 0; level < levels; ++level)
                addLevel(slice, level);
    }

    return Device::shared().makeTexture(descriptor, pixels.data());
}

struct Expected
{
    float r;
    float g;
    float b;
    float a;
};

Expected colourOf(int slice, int level)
{
    return {unorm(levelRed(level)), unorm(sliceGreen(slice)), 0.f, 1.f};
}

bool matches(const float* record, const Expected& expected)
{
    return near(record[0], expected.r) && near(record[1], expected.g)
           && near(record[2], expected.b) && near(record[3], expected.a);
}

template <typename Kernel>
Vector<float> runKernel(Kernel& kernel, int records)
{
    auto& device = Device::shared();
    auto output =
        device.makeBuffer((std::int64_t) records * 16, BufferUsage::Storage);
    kernel.output = output;
    kernel.prepare(device);
    check(kernel.pipeline().isValid());

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, 1);
    }

    commands.commit();

    auto values = Vector<float> {};
    values.resize(records * 4);
    output.read(values.data(), (std::int64_t) records * 16);
    return values;
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

// Twenty-three numbers: sizes at a level, level and layer counts.
struct QueryKernel final : ComputeProgram
{
    QueryKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        auto next = 0u;
        auto put = [&](const UInt& value) { write(output, at + next++, value); };
        auto putSize2 = [&](const UInt2& size)
        {
            put(size.x());
            put(size.y());
        };
        auto putSize3 = [&](const UInt3& size)
        {
            put(size.x());
            put(size.y());
            put(size.z());
        };

        putSize2(dimensions(image));
        putSize2(dimensions(image, 1));
        put(levelCount(image));
        putSize2(dimensions(layers, 2));
        put(layerCount(layers));
        put(levelCount(layers));
        putSize3(dimensions(volume));
        putSize3(dimensions(volume, 1));
        put(levelCount(volume));
        putSize2(dimensions(cube, 1));
        put(levelCount(cube));
        put(layerCount(cubes));
        putSize2(dimensions(cubes));
        put(levelCount(cubes));
    }

    Uniform<Texture2D> image;
    Uniform<Texture2DArray> layers;
    Uniform<Texture3D> volume;
    Uniform<TextureCube> cube;
    Uniform<TextureCubeArray> cubes;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(image, layers, volume, cube, cubes, output)
};

// The level each read names, on every kind with levels.
struct LevelKernel final : ComputeProgram
{
    LevelKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        auto next = 0u;
        auto put = [&](const Float4& value) { write(output, at + next++, value); };
        auto uv = float2(constant(0.5f), 0.5f);
        auto up = float3(constant(0.f), 1.f, 0.f);
        auto back = float3(constant(0.f), 0.f, -1.f);
        auto inside = float3(constant(0.5f), 0.5f, 0.3f);

        for (auto level = 0; level < 3; ++level)
            put(sampleLevel(layers, uv, 1, (float) level));

        for (auto level = 0; level < 3; ++level)
            put(sampleLevel(cube, up, (float) level));

        for (auto level = 0; level < 2; ++level)
            put(sampleLevel(cubes, back, 1, (float) level));

        for (auto level = 0; level < 3; ++level)
            put(sampleLevel(volume, inside, (float) level));
    }

    Uniform<Texture2DArray> layers;
    Uniform<TextureCube> cube;
    Uniform<TextureCubeArray> cubes;
    Uniform<Texture3D> volume;
    Uniform<OutputBuffer> output;

    EACP_SHADER(layers, cube, cubes, volume, output)
};

// A coordinate's derivatives of k texels select level log2(k).
struct GradKernel final : ComputeProgram
{
    GradKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        auto next = 0u;
        auto put = [&](const Float4& value) { write(output, at + next++, value); };
        auto uv = float2(constant(0.3f), 0.3f);
        auto inside = float3(constant(0.3f), 0.3f, 0.3f);

        for (auto texels: {1.f, 2.f, 4.f, 8.f})
            put(sampleGrad(image,
                           uv,
                           float2(constant(texels / 16.f), 0.f),
                           float2(constant(0.f), texels / 16.f)));

        put(sampleGrad(layers,
                       uv,
                       1,
                       float2(constant(4.f / 16.f), 0.f),
                       float2(constant(0.f), 4.f / 16.f)));

        for (auto texels: {2.f, 4.f})
            put(sampleGrad(volume,
                           inside,
                           float3(constant(texels / 8.f), 0.f, 0.f),
                           float3(constant(0.f), texels / 8.f, 0.f)));
    }

    Uniform<Texture2D> image;
    Uniform<Texture2DArray> layers;
    Uniform<Texture3D> volume;
    Uniform<OutputBuffer> output;

    EACP_SHADER(image, layers, volume, output)
};

struct GatherKernel final : ComputeProgram
{
    GatherKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        auto centre = float2(constant(0.5f), 0.5f);

        write(output, at, gather(quad, centre));
        write(output, at + 1u, gather(quad, centre, 1));
        write(output, at + 2u, gather(quadLayers, centre, 1, 2));
        write(output, at + 3u, gather(cubes, float3(constant(1.f), 0.f, 0.f), 1, 1));
    }

    Uniform<Texture2D> quad;
    Uniform<Texture2DArray> quadLayers;
    Uniform<TextureCubeArray> cubes;
    Uniform<OutputBuffer> output;

    EACP_SHADER(quad, quadLayers, cubes, output)
};

struct LoadKernel final : ComputeProgram
{
    LoadKernel() { compile(); }

    void define() override
    {
        auto at = threadId();
        auto texel = [&](unsigned x, unsigned y)
        { return uint2(unsignedInteger(x), unsignedInteger(y)); };
        auto voxel = [&](unsigned x, unsigned y, unsigned z)
        {
            return uint3(unsignedInteger(x), unsignedInteger(y), unsignedInteger(z));
        };
        auto signedTexel =
            int2(toInt(unsignedInteger(3u)), toInt(unsignedInteger(1u)));

        write(output, at, load(grid, signedTexel, 0));
        write(output, at + 1u, load(image, texel(1, 1), 1));
        write(output, at + 2u, load(image, texel(0, 0), 2));
        write(output, at + 3u, load(layers, texel(1, 1), 2, 1));
        write(output, at + 4u, load(volume, voxel(1, 2, 3), 0));
        write(output, at + 5u, load(volume, voxel(0, 1, 1), 1));
    }

    Uniform<Texture2D> grid;
    Uniform<Texture2D> image;
    Uniform<Texture2DArray> layers;
    Uniform<Texture3D> volume;
    Uniform<OutputBuffer> output;

    EACP_SHADER(grid, image, layers, volume, output)
};

constexpr auto biasSize = 16;

// One draw at one bias: red the planar level, green the layered one, blue the
// image of the volume an implicit sample reads, alpha the green of the cube
// array's face.
struct BiasShader final : ShaderProgram
{
    BiasShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto uv = fragmentCoordinate().xy() / (float) biasSize;

        setFragment(float4(sampleBias(image, uv, bias).x(),
                           sampleBias(layers, uv, 1, bias).x(),
                           sample(volume, float3(uv, 0.3f)).y(),
                           sample(cubes, float3(constant(0.f), 1.f, 0.f), 1).y()));
    }

    Uniform<Texture2D> image;
    Uniform<Texture2DArray> layers;
    Uniform<Texture3D> volume;
    Uniform<TextureCubeArray> cubes;
    Uniform<Float> bias;

    EACP_SHADER(image, layers, volume, cubes, bias)
};

struct DepthWriter final : ShaderProgram
{
    DepthWriter()
    {
        compile();
        setVertices(fullQuad, 6);
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position * extentX, depth, 1.f) + offset);
        setFragment(float4(depth, 0.f, 0.f, 1.f));
    }

    Uniform<Float> depth;
    Uniform<Float2> extentX;
    Uniform<Float4> offset;

    EACP_SHADER(depth, extentX, offset)
};

constexpr auto nearestCompare = TextureSampling {
    TextureFilter::Nearest, TextureAddressMode::Clamp, TextureCompare::LessEqual};

TextureDescriptor describeDepthTarget(TextureType kind, int layers = 1)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = 4;
    descriptor.height = 4;
    descriptor.kind = kind;
    descriptor.arrayLength = layers;
    descriptor.renderTarget = true;
    descriptor.sampleableDepth = true;
    return descriptor;
}

void prepareWriter(DepthWriter& writer)
{
    writer.prepare(1,
                   true,
                   PrimitiveTopology::Triangles,
                   BlendMode::None,
                   pixelFormatFor(TextureFormat::RGBA8Unorm));
}

void placeWriter(DepthWriter& writer, float depth, bool leftHalf = false)
{
    writer.depth = depth;
    writer.extentX = Array {leftHalf ? 0.5f : 1.f, 1.f};
    writer.offset = Array {leftHalf ? -0.5f : 0.f, 0.f, 0.f, 0.f};
}

// One slice of a target's depth plane, cleared and kept, with `draw` run in it.
void drawIntoSlice(Frame& frame,
                   const Texture& target,
                   int slice,
                   const std::function<void(RenderPass&)>& draw)
{
    auto colour = ColorTarget {&target};
    colour.slice = slice;

    auto descriptor = RenderPassDescriptor {};
    descriptor.depthAction = DepthAction::Keep;

    auto pass = frame.beginMultiTargetPass(std::span<const ColorTarget> {&colour, 1},
                                           descriptor);
    draw(pass);
}

void drawDepth(
    Frame& frame, const Texture& target, int slice, DepthWriter& writer, float depth)
{
    placeWriter(writer, depth);

    auto draw = [&](RenderPass& pass) { pass.draw(writer); };
    drawIntoSlice(frame, target, slice, draw);
}

template <typename Kernel>
Vector<float> runKernelInFrame(Kernel& kernel,
                               int records,
                               const std::function<void(Frame&)>& before)
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

// A 4x4 plane: 0.25 on its left half, 0.75 on its right.
struct PlaneKernel final : ComputeProgram
{
    PlaneKernel()
    {
        shadow.sampling = nearestCompare;
        compile();
    }

    void define() override
    {
        auto at = threadId();
        auto texel = [&](unsigned x, unsigned y)
        { return uint2(unsignedInteger(x), unsignedInteger(y)); };
        auto left = float2(constant(0.125f), 0.5f);
        auto right = float2(constant(0.875f), 0.5f);
        auto centre = float2(constant(0.5f), 0.5f);

        write(output,
              at,
              float4(load(plain, texel(0, 0), 0),
                     load(plain, texel(3, 0), 0),
                     sampleLevel(plain, left, 0),
                     sampleLevel(plain, right, 0)));
        write(output,
              at + 1u,
              float4(sampleCompareLevel(shadow, left, 0.5f),
                     sampleCompareLevel(shadow, right, 0.5f),
                     toFloat(levelCount(plain)),
                     toFloat(dimensions(plain).x())));
        write(output, at + 2u, gather(plain, centre));
        write(output, at + 3u, gatherCompare(shadow, centre, 0.5f));
    }

    Uniform<TextureDepth2D> plain;
    Uniform<TextureDepth2D> shadow;
    Uniform<OutputBuffer> output;

    EACP_SHADER(plain, shadow, output)
};

// +X, -X, +Y, -Y, +Z, -Z: the face order of a cube's slices.
constexpr float faceAxes[6][3] = {
    {1.f, 0.f, 0.f},
    {-1.f, 0.f, 0.f},
    {0.f, 1.f, 0.f},
    {0.f, -1.f, 0.f},
    {0.f, 0.f, 1.f},
    {0.f, 0.f, -1.f},
};

float faceDepth(int slice)
{
    return 0.05f * (float) (slice + 1);
}

// A cube whose face f is at faceDepth(f).
struct CubeKernel final : ComputeProgram
{
    CubeKernel()
    {
        shadow.sampling = nearestCompare;
        compile();
    }

    void define() override
    {
        auto at = threadId();
        auto face = [&](int which)
        {
            const auto* axis = faceAxes[which];
            return float3(constant(axis[0]), axis[1], axis[2]);
        };

        write(output,
              at,
              float4(sampleLevel(plain, face(0), 0),
                     sampleLevel(plain, face(1), 0),
                     sampleLevel(plain, face(2), 0),
                     sampleLevel(plain, face(3), 0)));
        write(output,
              at + 1u,
              float4(sampleLevel(plain, face(4), 0),
                     sampleLevel(plain, face(5), 0),
                     toFloat(dimensions(plain).x()),
                     toFloat(levelCount(plain))));
        write(output, at + 2u, gather(plain, face(4)));
        write(output,
              at + 3u,
              float4(sampleCompareLevel(shadow, face(0), faceDepth(0) + 0.02f),
                     sampleCompareLevel(shadow, face(1), faceDepth(0) + 0.02f),
                     gatherCompare(shadow, face(4), faceDepth(4) - 0.02f).x(),
                     gatherCompare(shadow, face(5), faceDepth(5) + 0.02f).w()));
    }

    Uniform<TextureDepthCube> plain;
    Uniform<TextureDepthCube> shadow;
    Uniform<OutputBuffer> output;

    EACP_SHADER(plain, shadow, output)
};

// The same cube read by a fragment shader, through the implicit forms.
struct CubeReader final : ShaderProgram
{
    CubeReader()
    {
        shadow.sampling = nearestCompare;
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto up = float3(constant(0.f), 1.f, 0.f);
        auto down = float3(constant(0.f), -1.f, 0.f);

        setFragment(float4(sample(plain, up),
                           sampleCompare(shadow, down, faceDepth(3) - 0.02f),
                           sampleCompare(shadow, up, faceDepth(2) + 0.02f),
                           1.f));
    }

    Uniform<TextureDepthCube> plain;
    Uniform<TextureDepthCube> shadow;

    EACP_SHADER(plain, shadow)
};

// Two cubes; slice s = cube * 6 + face is at faceDepth(s).
struct CubeArrayKernel final : ComputeProgram
{
    CubeArrayKernel()
    {
        shadow.sampling = nearestCompare;
        compile();
    }

    void define() override
    {
        auto at = threadId();
        auto face = [&](int which)
        {
            const auto* axis = faceAxes[which];
            return float3(constant(axis[0]), axis[1], axis[2]);
        };

        write(output,
              at,
              float4(sampleLevel(plain, face(3), 1, 0),
                     sampleLevel(plain, face(0), 0, 0),
                     sampleCompareLevel(shadow, face(3), 1, faceDepth(9) - 0.02f),
                     toFloat(layerCount(plain))));
        write(output, at + 1u, gather(plain, face(5), 0));
        write(output,
              at + 2u,
              gatherCompare(shadow, face(4), 1, faceDepth(10) - 0.02f));
    }

    Uniform<TextureDepthCubeArray> plain;
    Uniform<TextureDepthCubeArray> shadow;
    Uniform<OutputBuffer> output;

    EACP_SHADER(plain, shadow, output)
};

bool allNear(const float* record, float expected)
{
    return near(record[0], expected) && near(record[1], expected)
           && near(record[2], expected) && near(record[3], expected);
}
} // namespace

auto tQueries = test("TextureFunctionDevice/queriesReportSizesLevelsAndLayers") = []
{
    if (!canTestCubeArrays())
        return;

    auto image = makeLevelled(TextureType::TwoD, 8, 4, 4);
    auto layers = makeLevelled(TextureType::TwoDArray, 4, 4, 3, 3);
    auto volume = makeLevelled(TextureType::ThreeD, 8, 4, 3, 1, 2);
    auto cube = makeLevelled(TextureType::Cube, 4, 4, 3);
    auto cubes = makeLevelled(TextureType::CubeArray, 4, 4, 2, 2);

    for (const auto* texture: {&image, &layers, &volume, &cube, &cubes})
        check(texture->isValid());

    auto kernel = QueryKernel {};
    kernel.image = image;
    kernel.layers = layers;
    kernel.volume = volume;
    kernel.cube = cube;
    kernel.cubes = cubes;

    const auto words = runKernel(kernel, 6);

    constexpr std::uint32_t expected[] = {8, 4, 4, 2, 4, 1, 1, 3, 3, 8, 4, 2,
                                          4, 2, 1, 3, 2, 2, 3, 2, 4, 4, 2};

    for (auto i = 0; i < (int) std::size(expected); ++i)
        check(std::bit_cast<std::uint32_t>(words[i]) == expected[i],
              "query " + std::to_string(i));
};

auto tLevels = test("TextureFunctionDevice/sampleLevelReadsTheLevelNamed") = []
{
    if (!canTestCubeArrays())
        return;

    auto kernel = LevelKernel {};
    auto layers = makeLevelled(TextureType::TwoDArray, 4, 4, 3, 3);
    auto cube = makeLevelled(TextureType::Cube, 4, 4, 3);
    auto cubes = makeLevelled(TextureType::CubeArray, 4, 4, 2, 2);
    auto volume = makeLevelled(TextureType::ThreeD, 8, 8, 3, 1, 8);
    kernel.layers = layers;
    kernel.cube = cube;
    kernel.cubes = cubes;
    kernel.volume = volume;

    const auto values = runKernel(kernel, 11);
    const auto* record = values.data();

    for (auto level = 0; level < 3; ++level, record += 4)
        check(matches(record, colourOf(1, level)), "array level");

    for (auto level = 0; level < 3; ++level, record += 4)
        check(matches(record, colourOf(2, level)), "cube level");

    for (auto level = 0; level < 2; ++level, record += 4)
        check(matches(record, colourOf(11, level)), "cube array level");

    // z = 0.3 lands in image 2 of 8, 1 of 4 and 0 of 2.
    for (auto level = 0; level < 3; ++level, record += 4)
        check(matches(record, colourOf(2 >> level, level)), "volume level");
};

auto tGrad = test("TextureFunctionDevice/sampleGradPicksTheLevelOfItsGradients") = []
{
    if (!canTest())
        return;

    auto kernel = GradKernel {};
    auto image = makeLevelled(TextureType::TwoD, 16, 16, 5);
    auto layers = makeLevelled(TextureType::TwoDArray, 16, 16, 5, 2);
    auto volume = makeLevelled(TextureType::ThreeD, 8, 8, 4, 1, 8);
    kernel.image = image;
    kernel.layers = layers;
    kernel.volume = volume;

    const auto values = runKernel(kernel, 7);
    const auto* record = values.data();

    for (auto level = 0; level < 4; ++level, record += 4)
        check(matches(record, colourOf(0, level)), "2D gradient");

    check(matches(record, colourOf(1, 2)), "array gradient");
    record += 4;

    check(matches(record, colourOf(1, 1)), "volume gradient of 2");
    record += 4;

    check(matches(record, colourOf(0, 2)), "volume gradient of 4");
};

// WGSL's order, (umin, vmax), (umax, vmax), (umax, vmin), (umin, vmin), with v
// growing down the image - the texel below-left of the point first.
auto tGather =
    test("TextureFunctionDevice/gatherReturnsTheFourTexelsInWgslOrder") = []
{
    if (!canTestCubeArrays())
        return;

    auto quadDescriptor = TextureDescriptor {};
    quadDescriptor.width = 2;
    quadDescriptor.height = 2;

    const std::uint32_t quadTexels[] = {
        rgba(10, 110, 0), rgba(20, 120, 0), rgba(30, 130, 0), rgba(40, 140, 0)};
    auto quad = Device::shared().makeTexture(quadDescriptor, quadTexels);

    auto layersDescriptor = quadDescriptor;
    layersDescriptor.kind = TextureType::TwoDArray;
    layersDescriptor.arrayLength = 2;

    const std::uint32_t layerTexels[] = {rgba(0, 0, 1),
                                         rgba(0, 0, 2),
                                         rgba(0, 0, 3),
                                         rgba(0, 0, 4),
                                         rgba(0, 0, 60),
                                         rgba(0, 0, 70),
                                         rgba(0, 0, 80),
                                         rgba(0, 0, 90)};
    auto quadLayers = Device::shared().makeTexture(layersDescriptor, layerTexels);
    auto cubes = makeLevelled(TextureType::CubeArray, 4, 4, 1, 2);

    auto kernel = GatherKernel {};
    kernel.quad = quad;
    kernel.quadLayers = quadLayers;
    kernel.cubes = cubes;

    const auto values = runKernel(kernel, 4);

    check(matches(values.data(), {unorm(30), unorm(40), unorm(20), unorm(10)}));
    check(matches(values.data() + 4,
                  {unorm(130), unorm(140), unorm(120), unorm(110)}));
    check(matches(values.data() + 8, {unorm(80), unorm(90), unorm(70), unorm(60)}));
    check(allNear(values.data() + 12, unorm(sliceGreen(6))));
};

auto tLoads = test("TextureFunctionDevice/loadReadsLevelsLayersAndImages") = []
{
    if (!canTest())
        return;

    auto gridDescriptor = TextureDescriptor {};
    gridDescriptor.width = 4;
    gridDescriptor.height = 4;

    auto gridTexels = Vector<std::uint32_t> {};

    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 4; ++x)
            gridTexels.add(rgba(10 * x + 5, 10 * y + 5, 0));

    auto grid = Device::shared().makeTexture(gridDescriptor, gridTexels.data());

    auto kernel = LoadKernel {};
    auto image = makeLevelled(TextureType::TwoD, 4, 4, 3);
    auto layers = makeLevelled(TextureType::TwoDArray, 4, 4, 2, 3);
    auto volume = makeLevelled(TextureType::ThreeD, 4, 4, 2, 1, 4);
    kernel.grid = grid;
    kernel.image = image;
    kernel.layers = layers;
    kernel.volume = volume;

    const auto values = runKernel(kernel, 6);

    check(matches(values.data(), {unorm(35), unorm(15), 0.f, 1.f}));
    check(matches(values.data() + 4, colourOf(0, 1)));
    check(matches(values.data() + 8, colourOf(0, 2)));
    check(matches(values.data() + 12, colourOf(2, 1)));
    check(matches(values.data() + 16, colourOf(3, 0)));
    check(matches(values.data() + 20, colourOf(1, 1)));
};

// A fragment's uv steps one texel a pixel, so its derived level is 0 and the
// bias alone moves it.
auto tBias = test("TextureFunctionDevice/sampleBiasMovesTheDerivedLevel") = []
{
    if (!canTestCubeArrays())
        return;

    auto image = makeLevelled(TextureType::TwoD, biasSize, biasSize, 5);
    auto layers = makeLevelled(TextureType::TwoDArray, biasSize, biasSize, 5, 2);
    auto volume = makeLevelled(TextureType::ThreeD, 16, 16, 1, 1, 16);
    auto cubes = makeLevelled(TextureType::CubeArray, 4, 4, 1, 2);

    auto shader = BiasShader {};
    shader.image = image;
    shader.layers = layers;
    shader.volume = volume;
    shader.cubes = cubes;
    shader.setVertices(fullQuad, 6);
    shader.prepare(1,
                   false,
                   PrimitiveTopology::Triangles,
                   BlendMode::None,
                   pixelFormatFor(TextureFormat::RGBA32Float));

    auto targetDescriptor = TextureDescriptor {};
    targetDescriptor.width = biasSize;
    targetDescriptor.height = biasSize;
    targetDescriptor.format = TextureFormat::RGBA32Float;
    targetDescriptor.renderTarget = true;
    auto target = Device::shared().makeTexture(targetDescriptor);

    for (auto bias = 0; bias < 3; ++bias)
    {
        shader.bias = (float) bias;

        auto pixels = Vector<float> {};
        pixels.resize(biasSize * biasSize * 4);

        auto draw = [&](Frame& frame)
        {
            {
                auto pass = frame.beginPass(target);
                pass.draw(shader);
            }

            frame.flush();
            target.read(pixels.data());
        };

        runFrame(draw);

        const auto expected = Expected {unorm(levelRed(bias)),
                                        unorm(levelRed(bias)),
                                        unorm(sliceGreen(4)),
                                        unorm(sliceGreen(8))};

        auto everyPixel = true;

        for (auto i = 0; i < biasSize * biasSize; ++i)
            everyPixel = everyPixel && matches(pixels.data() + i * 4, expected);

        check(everyPixel, "bias " + std::to_string(bias));
    }
};

auto tPlane = test("TextureFunctionDevice/aDepthPlaneLoadsComparesAndGathers") = []
{
    if (!canTest())
        return;

    auto target =
        Device::shared().makeTexture(describeDepthTarget(TextureType::TwoD));
    check(target.hasSampleableDepth());

    auto back = DepthWriter {};
    auto front = DepthWriter {};
    prepareWriter(back);
    prepareWriter(front);

    auto kernel = PlaneKernel {};
    kernel.plain = target;
    kernel.shadow = target;

    placeWriter(back, 0.75f);
    placeWriter(front, 0.25f, true);

    auto drawBoth = [&](RenderPass& pass)
    {
        pass.draw(back);
        pass.draw(front);
    };

    auto before = [&](Frame& frame) { drawIntoSlice(frame, target, 0, drawBoth); };

    const auto values = runKernelInFrame(kernel, 4, before);

    check(matches(values.data(), {0.25f, 0.75f, 0.25f, 0.75f}));
    check(matches(values.data() + 4, {0.f, 1.f, 1.f, 4.f}));
    check(matches(values.data() + 8, {0.25f, 0.75f, 0.75f, 0.25f}));
    check(matches(values.data() + 12, {0.f, 1.f, 1.f, 0.f}));
};

auto tCube = test("TextureFunctionDevice/aDepthCubeReadsEachFace") = []
{
    if (!canTest())
        return;

    auto target =
        Device::shared().makeTexture(describeDepthTarget(TextureType::Cube));
    check(target.hasSampleableDepth());

    auto writer = DepthWriter {};
    prepareWriter(writer);

    auto drawFaces = [&](Frame& frame)
    {
        for (auto face = 0; face < 6; ++face)
            drawDepth(frame, target, face, writer, faceDepth(face));
    };

    auto kernel = CubeKernel {};
    kernel.plain = target;
    kernel.shadow = target;

    const auto values = runKernelInFrame(kernel, 4, drawFaces);

    check(matches(values.data(),
                  {faceDepth(0), faceDepth(1), faceDepth(2), faceDepth(3)}));
    check(matches(values.data() + 4, {faceDepth(4), faceDepth(5), 4.f, 1.f}));
    check(allNear(values.data() + 8, faceDepth(4)));
    check(matches(values.data() + 12, {0.f, 1.f, 1.f, 0.f}));

    auto reader = CubeReader {};
    reader.plain = target;
    reader.shadow = target;
    reader.setVertices(fullQuad, 6);
    reader.prepare(1,
                   false,
                   PrimitiveTopology::Triangles,
                   BlendMode::None,
                   pixelFormatFor(TextureFormat::RGBA32Float));

    auto copyDescriptor = TextureDescriptor {};
    copyDescriptor.width = 4;
    copyDescriptor.height = 4;
    copyDescriptor.format = TextureFormat::RGBA32Float;
    copyDescriptor.renderTarget = true;
    auto copy = Device::shared().makeTexture(copyDescriptor);

    auto pixels = Vector<float> {};
    pixels.resize(4 * 4 * 4);

    auto drawAndRead = [&](Frame& frame)
    {
        drawFaces(frame);

        {
            auto pass = frame.beginPass(copy);
            pass.draw(reader);
        }

        frame.flush();
        copy.read(pixels.data());
    };

    runFrame(drawAndRead);
    check(matches(pixels.data(), {faceDepth(2), 1.f, 0.f, 1.f}));
};

auto tCubeArray = test("TextureFunctionDevice/aDepthCubeArrayReadsEachCube") = []
{
    if (!canTestCubeArrays())
        return;

    auto target =
        Device::shared().makeTexture(describeDepthTarget(TextureType::CubeArray, 2));
    check(target.hasSampleableDepth());

    auto writer = DepthWriter {};
    prepareWriter(writer);

    auto drawSlices = [&](Frame& frame)
    {
        for (auto slice = 0; slice < 12; ++slice)
            drawDepth(frame, target, slice, writer, faceDepth(slice));
    };

    auto kernel = CubeArrayKernel {};
    kernel.plain = target;
    kernel.shadow = target;

    const auto values = runKernelInFrame(kernel, 3, drawSlices);

    check(matches(values.data(), {faceDepth(9), faceDepth(0), 1.f, 2.f}));
    check(allNear(values.data() + 4, faceDepth(5)));
    check(allNear(values.data() + 8, 1.f));
};
} // namespace TextureFunctionDeviceTests
