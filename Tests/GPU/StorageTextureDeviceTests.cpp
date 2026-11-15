#include "Common.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

// Storage textures on the device: a texel read, changed and written back
// through one bind, a second read in the same thread seeing the first write;
// layers of an array, images of a volume and the faces of a cube written by
// position; unsigned and signed formats; read-only loads that need no
// computeWrite; the queries; a kernel refused for a format the device cannot
// read and write; and a fragment shader loading a Read storage texture.
//
// Self-skips without a device, each read-write case where the device does not
// take its format.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace StorageTextureDeviceTests
{
namespace
{
struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace StorageTextureDeviceTests

EACP_SHADER_VALUE(StorageTextureDeviceTests::QuadVertex, Float2)

namespace StorageTextureDeviceTests
{
namespace
{
constexpr auto side = 4;
constexpr auto texelsPerImage = side * side;

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

bool canReadWrite(TextureFormat format)
{
    return canTest() && Device::shared().supportsComputeReadWrite(format);
}

struct Shape
{
    TextureType kind = TextureType::TwoD;
    int layers = 1;
    int depth = 1;
};

Texture makeTexture(TextureFormat format,
                    const Shape& shape = {},
                    const void* pixels = nullptr,
                    bool computeWrite = true)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = side;
    descriptor.height = side;
    descriptor.format = format;
    descriptor.kind = shape.kind;
    descriptor.arrayLength = shape.layers;
    descriptor.volumeDepth = shape.depth;
    descriptor.computeWrite = computeWrite;
    return Device::shared().makeTexture(descriptor, pixels);
}

template <typename Kernel>
void dispatch(Kernel& kernel, int width, int height, int depth = 1)
{
    auto& device = Device::shared();
    kernel.prepare(device);
    check(kernel.isValid());

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();

        if (depth == 1)
            pass.dispatch(kernel, width, height);
        else
            pass.dispatch(kernel, width, height, depth);
    }

    commands.commit();
}

// One image of a slice or of a volume, tightly packed.
template <typename T>
Vector<T> readImage(const Texture& texture, int image, int valuesPerTexel)
{
    const auto volume = texture.kind() == TextureType::ThreeD;
    auto region = TextureRegion::wholeLevel(0, volume ? 0 : image);

    if (volume)
    {
        region.origin.z = image;
        region.size.depth = 1;
    }

    auto values = Vector<T> {};
    values.resize(texelsPerImage * valuesPerTexel);
    texture.readSubresource(region, values.data());
    return values;
}

float seedAt(int x, int y)
{
    return (float) (x + side * y) / 8.f;
}

Vector<float> seeds()
{
    auto values = Vector<float> {};

    for (auto y = 0; y < side; ++y)
        for (auto x = 0; x < side; ++x)
            values.add(seedAt(x, y));

    return values;
}

// Doubles and adds one, then reads the texel back and adds a half: the second
// read is after the first write in the same thread, which is what a Metal
// read_write texture needs a fence between.
struct RewriteHeights final : ComputeProgram
{
    RewriteHeights() { compile(); }

    void define() override
    {
        auto p = toInt(threadId2());
        write(heights, p, load(heights, p) * 2.f + 1.f);
        write(heights, p, load(heights, p) + 0.5f);
    }

    Uniform<StorageTexture2D<TextureFormat::R32Float, StorageAccess::ReadWrite>>
        heights;

    EACP_SHADER(heights)
};

struct InvertColours final : ComputeProgram
{
    InvertColours() { compile(); }

    void define() override
    {
        auto p = threadId2();
        auto colour = load(image, p);
        write(image, p, float4(1.f - colour.x(), colour.y(), colour.z(), 1.f));
    }

    Uniform<StorageTexture2D<TextureFormat::RGBA8Unorm, StorageAccess::ReadWrite>>
        image;

    EACP_SHADER(image)
};

struct RewriteIntegers final : ComputeProgram
{
    RewriteIntegers() { compile(); }

    void define() override
    {
        auto p = toInt(threadId2());
        write(labels, p, load(labels, p) * 3u + 1u);
        write(signs, p, -load(signs, p) + 2);
    }

    Uniform<StorageTexture2D<TextureFormat::R32Uint, StorageAccess::ReadWrite>>
        labels;
    Uniform<StorageTexture2D<TextureFormat::R32Sint, StorageAccess::ReadWrite>>
        signs;

    EACP_SHADER(labels, signs)
};

struct WriteIntegers final : ComputeProgram
{
    WriteIntegers() { compile(); }

    void define() override
    {
        auto p = threadId2();
        auto x = p.x();
        auto y = p.y();
        write(bytes, p, uint4(x, y, x + y, unsignedInteger(7u)));
        write(halves, p, int4(-toInt(x), toInt(y) - 3, 0, 0));
    }

    Uniform<StorageTexture2D<TextureFormat::RGBA8Uint>> bytes;
    Uniform<StorageTexture2D<TextureFormat::RG16Sint>> halves;

    EACP_SHADER(bytes, halves)
};

// Each layer at its own z, each texel its own coordinates.
struct WriteLayers final : ComputeProgram
{
    WriteLayers() { compile(); }

    void define() override
    {
        auto id = threadId3();
        auto layer = id.z();
        write(layers, id.xy(), layer, float4(toFloat(id), 1.f));
        write(tally, id.xy(), layer, load(tally, id.xy(), layer) + layer * 10u);
    }

    Uniform<StorageTexture2DArray<TextureFormat::RGBA32Float>> layers;
    Uniform<StorageTexture2DArray<TextureFormat::R32Uint, StorageAccess::ReadWrite>>
        tally;

    EACP_SHADER(layers, tally)
};

struct WriteVolume final : ComputeProgram
{
    WriteVolume() { compile(); }

    void define() override
    {
        auto id = threadId3();
        write(colours, id, float4(toFloat(id) * (1.f / 255.f), 1.f));
        write(depths, id, load(depths, id) + toFloat(id.z()));
    }

    Uniform<StorageTexture3D<TextureFormat::RGBA8Unorm>> colours;
    Uniform<StorageTexture3D<TextureFormat::R32Float, StorageAccess::ReadWrite>>
        depths;

    EACP_SHADER(colours, depths)
};

struct WriteCubeFaces final : ComputeProgram
{
    WriteCubeFaces() { compile(); }

    void define() override
    {
        auto id = threadId3();
        auto red = toFloat(id.z()) * (10.f / 255.f);
        write(faces, id.xy(), id.z(), float4(red, 0.f, 0.f, 1.f));
    }

    Uniform<StorageTexture2DArray<TextureFormat::RGBA8Unorm>> faces;

    EACP_SHADER(faces)
};

// Read-only loads into a buffer: a 2D of ids, a layer of an array, a voxel of
// a signed volume.
struct ReadOnlyLoads final : ComputeProgram
{
    ReadOnlyLoads() { compile(); }

    void define() override
    {
        auto p = threadId2();
        auto at = p.y() * unsignedInteger((unsigned) side) + p.x();
        auto voxel = uint3(p.x(), p.y(), unsignedInteger(1u));

        write(output, at * 3u, load(ids, p).x());
        write(output, at * 3u + 1u, load(layers, p, 1).y());
        write(output, at * 3u + 2u, toUInt(load(field, voxel).x() + 100));
    }

    Uniform<StorageTexture2D<TextureFormat::R32Uint, StorageAccess::Read>> ids;
    Uniform<StorageTexture2DArray<TextureFormat::RGBA8Uint, StorageAccess::Read>>
        layers;
    Uniform<StorageTexture3D<TextureFormat::RGBA32Sint, StorageAccess::Read>> field;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(ids, layers, field, output)
};

struct Queries final : ComputeProgram
{
    Queries() { compile(); }

    void define() override
    {
        auto at = threadId2().x();
        auto next = 0u;
        auto put = [&](const UInt& value) { write(output, at + next++, value); };

        put(dimensions(plane).x());
        put(dimensions(plane).y());
        put(layerCount(layers));
        put(dimensions(layers).x());
        put(dimensions(volume).z());
        put(dimensions(readable).y());
    }

    Uniform<StorageTexture2D<TextureFormat::R32Float, StorageAccess::ReadWrite>>
        plane;
    Uniform<StorageTexture2DArray<TextureFormat::RGBA8Unorm>> layers;
    Uniform<StorageTexture3D<TextureFormat::RGBA16Float>> volume;
    Uniform<StorageTexture2D<TextureFormat::RGBA8Unorm, StorageAccess::Read>>
        readable;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(plane, layers, volume, readable, output)
};

// RG16Float reads and writes through one bind on no Metal tier, and on some
// D3D12 and Vulkan devices; the kernel is refused wherever the device says so.
struct TwoChannelHalves final : ComputeProgram
{
    TwoChannelHalves() { compile(); }

    void define() override
    {
        auto p = threadId2();
        write(image, p, load(image, p) * 2.f);
    }

    Uniform<StorageTexture2D<TextureFormat::RG16Float, StorageAccess::ReadWrite>>
        image;

    EACP_SHADER(image)
};

struct ShowIds final : ShaderProgram
{
    ShowIds() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto id = load(ids, toInt(fragmentCoordinate().xy()));
        setFragment(float4(toFloat(id.x()) * (1.f / 255.f), 0.f, 0.f, 1.f));
    }

    Uniform<StorageTexture2D<TextureFormat::R32Uint, StorageAccess::Read>> ids;

    EACP_SHADER(ids)
};

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

bool near(float value, float expected, float tolerance = 1.5e-3f)
{
    return std::fabs(value - expected) <= tolerance;
}

float unorm(std::uint8_t value)
{
    return (float) value / 255.f;
}
} // namespace

auto tReadModifyWrite =
    test("StorageTextureDevice/aKernelReadsAndRewritesATexelThroughOneBind") = []
{
    if (!canReadWrite(TextureFormat::R32Float))
        return;

    const auto seed = seeds();
    auto heights = makeTexture(TextureFormat::R32Float, {}, seed.data());
    check(heights.isValid());

    auto kernel = RewriteHeights {};
    kernel.heights = heights;
    dispatch(kernel, side, side);

    auto values = Vector<float> {};
    values.resize(texelsPerImage);
    heights.read(values.data());

    for (auto i = 0; i < texelsPerImage; ++i)
        check(values[i] == seed[i] * 2.f + 1.5f, std::to_string(i));
};

auto tReadModifyWriteColour = test(
    "StorageTextureDevice/aFourChannelUnormReadsAndWritesWhereTheDeviceDoes") = []
{
    if (!canReadWrite(TextureFormat::RGBA8Unorm))
        return;

    auto pixels = Vector<std::uint32_t> {};

    for (auto i = 0; i < texelsPerImage; ++i)
        pixels.add(0xff000000u | (std::uint32_t) (i * 8) | (40u << 8));

    auto image = makeTexture(TextureFormat::RGBA8Unorm, {}, pixels.data());
    auto kernel = InvertColours {};
    kernel.image = image;
    dispatch(kernel, side, side);

    auto bytes = Vector<std::uint8_t> {};
    bytes.resize(texelsPerImage * 4);
    image.read(bytes.data());

    for (auto i = 0; i < texelsPerImage; ++i)
    {
        check(
            near(unorm(bytes[i * 4]), 1.f - unorm((std::uint8_t) (i * 8)), 2.5e-3f));
        check(bytes[i * 4 + 1] == 40);
        check(bytes[i * 4 + 3] == 255);
    }
};

auto tReadModifyWriteIntegers =
    test("StorageTextureDevice/unsignedAndSignedTexelsReadAndWriteBack") = []
{
    if (!canReadWrite(TextureFormat::R32Uint)
        || !canReadWrite(TextureFormat::R32Sint))
        return;

    auto labelSeed = Vector<std::uint32_t> {};
    auto signSeed = Vector<std::int32_t> {};

    for (auto i = 0; i < texelsPerImage; ++i)
    {
        labelSeed.add((std::uint32_t) i * 11u + 5u);
        signSeed.add(i * 7 - 50);
    }

    auto labels = makeTexture(TextureFormat::R32Uint, {}, labelSeed.data());
    auto signs = makeTexture(TextureFormat::R32Sint, {}, signSeed.data());

    auto kernel = RewriteIntegers {};
    kernel.labels = labels;
    kernel.signs = signs;
    dispatch(kernel, side, side);

    auto labelValues = Vector<std::uint32_t> {};
    labelValues.resize(texelsPerImage);
    labels.read(labelValues.data());

    auto signValues = Vector<std::int32_t> {};
    signValues.resize(texelsPerImage);
    signs.read(signValues.data());

    for (auto i = 0; i < texelsPerImage; ++i)
    {
        check(labelValues[i] == labelSeed[i] * 3u + 1u);
        check(signValues[i] == -signSeed[i] + 2);
    }
};

auto tWriteIntegers = test(
    "StorageTextureDevice/aKernelWritesEightBitUnsignedAndSixteenBitSigned") = []
{
    if (!canTest()
        || !Device::shared().supportsComputeWrite(TextureFormat::RGBA8Uint)
        || !Device::shared().supportsComputeWrite(TextureFormat::RG16Sint))
        return;

    auto bytes = makeTexture(TextureFormat::RGBA8Uint);
    auto halves = makeTexture(TextureFormat::RG16Sint);

    auto kernel = WriteIntegers {};
    kernel.bytes = bytes;
    kernel.halves = halves;
    dispatch(kernel, side, side);

    auto byteValues = Vector<std::uint8_t> {};
    byteValues.resize(texelsPerImage * 4);
    bytes.read(byteValues.data());

    auto halfValues = Vector<std::int16_t> {};
    halfValues.resize(texelsPerImage * 2);
    halves.read(halfValues.data());

    for (auto y = 0; y < side; ++y)
        for (auto x = 0; x < side; ++x)
        {
            const auto i = y * side + x;
            check(byteValues[i * 4] == x && byteValues[i * 4 + 1] == y);
            check(byteValues[i * 4 + 2] == x + y && byteValues[i * 4 + 3] == 7);
            check(halfValues[i * 2] == -x && halfValues[i * 2 + 1] == y - 3);
        }
};

auto tLayers =
    test("StorageTextureDevice/eachLayerOfAnArrayIsWrittenByPosition") = []
{
    if (!canReadWrite(TextureFormat::R32Uint))
        return;

    constexpr auto layerCount = 3;
    const auto shape = Shape {TextureType::TwoDArray, layerCount};

    auto tallySeed = Vector<std::uint32_t> {};
    tallySeed.resize(texelsPerImage * layerCount, 4u);

    auto layers = makeTexture(TextureFormat::RGBA32Float, shape);
    auto tally = makeTexture(TextureFormat::R32Uint, shape, tallySeed.data());

    auto kernel = WriteLayers {};
    kernel.layers = layers;
    kernel.tally = tally;
    dispatch(kernel, side, side, layerCount);

    for (auto layer = 0; layer < layerCount; ++layer)
    {
        const auto colours = readImage<float>(layers, layer, 4);
        const auto counts = readImage<std::uint32_t>(tally, layer, 1);

        for (auto y = 0; y < side; ++y)
            for (auto x = 0; x < side; ++x)
            {
                const auto i = y * side + x;
                check(colours[i * 4] == (float) x
                      && colours[i * 4 + 1] == (float) y);
                check(colours[i * 4 + 2] == (float) layer);
                check(counts[i] == 4u + (std::uint32_t) layer * 10u);
            }
    }
};

auto tVolume =
    test("StorageTextureDevice/eachImageOfAVolumeIsWrittenByPosition") = []
{
    if (!canReadWrite(TextureFormat::R32Float))
        return;

    constexpr auto depth = 2;
    const auto shape = Shape {TextureType::ThreeD, 1, depth};

    auto depthSeed = Vector<float> {};
    depthSeed.resize(texelsPerImage * depth, 0.25f);

    auto colours = makeTexture(TextureFormat::RGBA8Unorm, shape);
    auto depths = makeTexture(TextureFormat::R32Float, shape, depthSeed.data());

    auto kernel = WriteVolume {};
    kernel.colours = colours;
    kernel.depths = depths;
    dispatch(kernel, side, side, depth);

    for (auto z = 0; z < depth; ++z)
    {
        const auto bytes = readImage<std::uint8_t>(colours, z, 4);
        const auto values = readImage<float>(depths, z, 1);

        for (auto y = 0; y < side; ++y)
            for (auto x = 0; x < side; ++x)
            {
                const auto i = y * side + x;
                check(bytes[i * 4] == x && bytes[i * 4 + 1] == y);
                check(bytes[i * 4 + 2] == z && bytes[i * 4 + 3] == 255);
                check(values[i] == 0.25f + (float) z);
            }
    }
};

auto tCubeFaces =
    test("StorageTextureDevice/aCubeIsWrittenAsTheArrayOfItsFaces") = []
{
    if (!canTest())
        return;

    auto cube = makeTexture(TextureFormat::RGBA8Unorm, {TextureType::Cube});
    check(cube.isValid());

    auto kernel = WriteCubeFaces {};
    kernel.faces = cube;
    dispatch(kernel, side, side, 6);

    for (auto face = 0; face < 6; ++face)
    {
        const auto bytes = readImage<std::uint8_t>(cube, face, 4);

        for (auto i = 0; i < texelsPerImage; ++i)
            check(bytes[i * 4] == face * 10, "face " + std::to_string(face));
    }
};

// None of these was created with computeWrite: a read-only storage texture
// loads through the read binding, which every texture has.
auto tReadOnly =
    test("StorageTextureDevice/readOnlyStorageLoadsNeedNoComputeWrite") = []
{
    if (!canTest())
        return;

    auto ids = Vector<std::uint32_t> {};
    auto layerBytes = Vector<std::uint8_t> {};
    auto voxels = Vector<std::int32_t> {};

    for (auto i = 0; i < texelsPerImage; ++i)
        ids.add((std::uint32_t) i * 1000003u + 9u);

    for (auto layer = 0; layer < 2; ++layer)
        for (auto i = 0; i < texelsPerImage; ++i)
            for (auto channel = 0; channel < 4; ++channel)
                layerBytes.add((std::uint8_t) (layer * 100 + channel * 10 + i));

    for (auto z = 0; z < 2; ++z)
        for (auto i = 0; i < texelsPerImage; ++i)
            for (auto channel = 0; channel < 4; ++channel)
                voxels.add(z * 1000 - i - 40 + channel);

    auto kernel = ReadOnlyLoads {};
    auto idTexture = makeTexture(TextureFormat::R32Uint, {}, ids.data(), false);
    auto layerTexture = makeTexture(TextureFormat::RGBA8Uint,
                                    {TextureType::TwoDArray, 2},
                                    layerBytes.data(),
                                    false);
    auto fieldTexture = makeTexture(TextureFormat::RGBA32Sint,
                                    {TextureType::ThreeD, 1, 2},
                                    voxels.data(),
                                    false);
    auto output =
        Device::shared().makeBuffer(texelsPerImage * 3 * 4, BufferUsage::Storage);

    check(idTexture.isValid() && layerTexture.isValid() && fieldTexture.isValid());

    kernel.ids = idTexture;
    kernel.layers = layerTexture;
    kernel.field = fieldTexture;
    kernel.output = output;
    dispatch(kernel, side, side);

    auto values = Vector<std::uint32_t> {};
    values.resize(texelsPerImage * 3);
    output.read(values.data(), texelsPerImage * 3 * 4);

    for (auto i = 0; i < texelsPerImage; ++i)
    {
        check(values[i * 3] == ids[i]);
        check(values[i * 3 + 1] == (std::uint32_t) (100 + 10 + i));
        check(values[i * 3 + 2] == (std::uint32_t) (1000 - i - 40 + 100));
    }
};

auto tQueries = test("StorageTextureDevice/dimensionsAndLayersOfEveryShape") = []
{
    if (!canReadWrite(TextureFormat::R32Float)
        || !Device::shared().supportsComputeWrite(TextureFormat::RGBA16Float))
        return;

    auto plane = makeTexture(TextureFormat::R32Float);
    auto layers =
        makeTexture(TextureFormat::RGBA8Unorm, {TextureType::TwoDArray, 3});
    auto volume =
        makeTexture(TextureFormat::RGBA16Float, {TextureType::ThreeD, 1, 5});
    auto readable = makeTexture(TextureFormat::RGBA8Unorm, {}, nullptr, false);
    auto output = Device::shared().makeBuffer(6 * 4, BufferUsage::Storage);

    auto kernel = Queries {};
    kernel.plane = plane;
    kernel.layers = layers;
    kernel.volume = volume;
    kernel.readable = readable;
    kernel.output = output;
    dispatch(kernel, 1, 1);

    auto values = Vector<std::uint32_t> {};
    values.resize(6);
    output.read(values.data(), 6 * 4);

    check(values[0] == side && values[1] == side);
    check(values[2] == 3u && values[3] == side);
    check(values[4] == 5u && values[5] == side);
};

auto tRefused =
    test("StorageTextureDevice/aFormatTheDeviceCannotReadAndWriteIsRefused") = []
{
    if (!canTest())
        return;

    auto& device = Device::shared();
    const auto takes = device.supportsComputeReadWrite(TextureFormat::RG16Float);

    auto kernel = TwoChannelHalves {};
    check(kernel.fitsStorageTextures(device) == takes);

    kernel.prepare(device);
    check(kernel.isValid() == takes);

    if constexpr (Platform::isApple())
        check(!takes);
};

auto tMemberFits =
    test("StorageTextureDevice/aMemberBindsOnlyItsFormatAndShape") = []
{
    if (!canTest())
        return;

    constexpr auto plane = StorageTextureLayout {
        TextureKind::Texture2D, TextureFormat::RGBA8Unorm, TextureAccess::Write};
    constexpr auto layered = StorageTextureLayout {TextureKind::Texture2DArray,
                                                   TextureFormat::RGBA8Unorm,
                                                   TextureAccess::Write};

    auto image = makeTexture(TextureFormat::RGBA8Unorm);
    auto halves = makeTexture(TextureFormat::RGBA16Float);
    auto array = makeTexture(TextureFormat::RGBA8Unorm, {TextureType::TwoDArray, 2});
    auto cube = makeTexture(TextureFormat::RGBA8Unorm, {TextureType::Cube});

    check(detail::fitsStorageMember(image, plane));
    check(!detail::fitsStorageMember(halves, plane));
    check(!detail::fitsStorageMember(array, plane));
    check(detail::fitsStorageMember(array, layered));
    check(detail::fitsStorageMember(cube, layered));
    check(!detail::fitsStorageMember(image, layered));
};

auto tFragmentRead =
    test("StorageTextureDevice/aFragmentShaderLoadsAReadStorageTexture") = []
{
    if (!canTest())
        return;

    auto ids = Vector<std::uint32_t> {};

    for (auto i = 0; i < texelsPerImage; ++i)
        ids.add((std::uint32_t) i * 13u + 3u);

    auto idTexture = makeTexture(TextureFormat::R32Uint, {}, ids.data(), false);

    auto shader = ShowIds {};
    shader.ids = idTexture;
    shader.setVertices(fullQuad, 6);
    shader.prepare(1,
                   false,
                   PrimitiveTopology::Triangles,
                   BlendMode::None,
                   pixelFormatFor(TextureFormat::RGBA8Unorm));

    auto targetDescriptor = TextureDescriptor {};
    targetDescriptor.width = side;
    targetDescriptor.height = side;
    targetDescriptor.renderTarget = true;
    auto target = Device::shared().makeTexture(targetDescriptor);

    auto bytes = Vector<std::uint8_t> {};
    bytes.resize(texelsPerImage * 4);

    auto draw = [&](Frame& frame)
    {
        {
            auto pass = frame.beginPass(target);
            pass.draw(shader);
        }

        frame.flush();
        target.read(bytes.data());
    };

    runFrame(draw);

    for (auto i = 0; i < texelsPerImage; ++i)
        check(bytes[i * 4] == ids[i], std::to_string(i));
};
} // namespace StorageTextureDeviceTests
