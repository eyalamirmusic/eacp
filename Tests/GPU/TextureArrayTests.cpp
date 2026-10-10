#include "Common.h"

#include <functional>

// TextureType: 2D arrays, volumes and cube arrays, caller-built chains on every
// kind, and filling, updating and reading any level of any slice. Every texel
// is a distinct value, so one from the wrong layer, face, image or level reads
// back as the wrong number rather than as a plausible one. The sampling shaders
// are hand-written, the way bgfx's are: column n of an n-wide target samples
// layer, image or face n.
//
// Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
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

struct Dialects
{
    std::string mslDeclaration;
    std::string mslSample;
    std::string hlslDeclaration;
    std::string hlslSample;
    std::string glslDeclaration;
    std::string glslSample;
};

constexpr auto cubeDirections = R"(
    float3(1.0, 0.0, 0.0), float3(-1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0), float3(0.0, -1.0, 0.0),
    float3(0.0, 0.0, 1.0), float3(0.0, 0.0, -1.0))";

std::string mslColumns(const Dialects& dialects)
{
    return R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
};

struct VertexOut
{
    float4 position [[position]];
};

constant float3 directions[6] = {)"
           + std::string {cubeDirections} + R"(};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             )"
           + dialects.mslDeclaration + R"( image [[texture(0)]],
                             sampler imageSampler [[sampler(0)]])
{
    uint column = uint(in.position.x);
    return )"
           + dialects.mslSample + R"(;
}
)";
}

std::string hlslColumns(const Dialects& dialects)
{
    return dialects.hlslDeclaration + R"( image : register(t0);
SamplerState imageSampler : register(s8);

static const float3 directions[6] = {)"
           + std::string {cubeDirections} + R"(};

float4 vertexMain(float2 position : TEXCOORD0) : SV_Position
{
    return float4(position, 0.0, 1.0);
}

float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    uint column = uint(position.x);
    return )"
           + dialects.hlslSample + R"(;
}
)";
}

std::string glslColumns(const Dialects& dialects)
{
    return R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(set = 0, binding = 8) uniform )"
           + dialects.glslDeclaration + R"( image;
layout(location = 0) out vec4 fragColor;

const vec3 directions[6] = vec3[6](
    vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
    vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));

void main()
{
    uint column = uint(gl_FragCoord.x);
    fragColor = )"
           + dialects.glslSample + R"(;
}
#endif
)";
}

Dialects arrayDialects()
{
    return {"texture2d_array<float>",
            "image.sample(imageSampler, float2(0.5), column)",
            "Texture2DArray<float4>",
            "image.Sample(imageSampler, float3(0.5, 0.5, column))",
            "sampler2DArray",
            "texture(image, vec3(0.5, 0.5, float(column)))"};
}

Dialects volumeDialects(int depth)
{
    const auto w = "(float(column) + 0.5) / " + std::to_string(depth) + ".0";

    return {"texture3d<float>",
            "image.sample(imageSampler, float3(0.5, 0.5, " + w + "))",
            "Texture3D<float4>",
            "image.Sample(imageSampler, float3(0.5, 0.5, " + w + "))",
            "sampler3D",
            "texture(image, vec3(0.5, 0.5, " + w + "))"};
}

Dialects cubeArrayDialects()
{
    return {"texturecube_array<float>",
            "image.sample(imageSampler, directions[column % 6], column / 6)",
            "TextureCubeArray<float4>",
            "image.Sample(imageSampler, float4(directions[column % 6], column / 6))",
            "samplerCubeArray",
            "texture(image, vec4(directions[column % 6u], float(column / 6u)))"};
}

ShaderLibrary makeColumnsLibrary(const Dialects& dialects)
{
    auto source = nativeShaderSource(
        mslColumns(dialects), hlslColumns(dialects), glslColumns(dialects));

    return Device::shared().makeShaderLibrary(
        source.withVertex("vertexMain").withFragment("fragmentMain"));
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

Texture makeTarget(int width, int height = 1)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.renderTarget = true;
    return Device::shared().makeTexture(descriptor);
}

Vector<std::uint32_t> readAll(const Texture& texture)
{
    auto values = Vector<std::uint32_t> {};
    values.resize((std::size_t) (texture.width() * texture.height()));
    texture.read(values.data());
    return values;
}

// Column n of a `columns`-wide target, sampling `image` as the library says.
Vector<std::uint32_t>
    drawColumns(const Dialects& dialects, const Texture& image, int columns)
{
    auto library = makeColumnsLibrary(dialects);

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
    descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
    descriptor.vertexLayout.stride = sizeof(PositionVertex);

    auto pipeline = Device::shared().makeRenderPipeline(descriptor);
    auto vertices = Device::shared().makeBuffer(fullQuad);
    auto sampler = Device::shared().makeSampler({});
    auto target = makeTarget(columns);

    check(pipeline.isValid() && target.isValid());

    runFrame(
        [&](Frame& frame)
        {
            auto pass = frame.beginPass(target);
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(vertices);
            pass.setFragmentTexture(image, 0, sampler);
            pass.draw(6);
        });

    return readAll(target);
}

// A distinct opaque RGBA8 value for each slice (or image), level and texel.
std::uint32_t texelValue(int slice, int level, int index)
{
    return 0xff000000u | ((std::uint32_t) slice << 16)
           | ((std::uint32_t) level << 12) | (std::uint32_t) (index & 0xfff);
}

std::uint32_t sliceColor(int slice)
{
    return texelValue(slice, 0, 0);
}

Vector<std::uint32_t> filled(int count, std::uint32_t value)
{
    auto values = Vector<std::uint32_t> {};

    for (auto index = 0; index < count; ++index)
        values.add(value);

    return values;
}

Vector<std::uint32_t> levelTexels(int slice, int level, int count)
{
    auto values = Vector<std::uint32_t> {};

    for (auto index = 0; index < count; ++index)
        values.add(texelValue(slice, level, index));

    return values;
}

int extent(int size, int level)
{
    return std::max(1, size >> level);
}

Vector<std::uint32_t> readLevel(const Texture& texture, int level, int slice)
{
    const auto width = extent(texture.width(), level);
    const auto height = extent(texture.height(), level);
    const auto depth = extent(texture.volumeDepth(), level);

    auto values = Vector<std::uint32_t> {};
    values.resize((std::size_t) (width * height * depth));
    texture.readSubresource(TextureRegion::wholeLevel(level, slice), values.data());
    return values;
}

// Every slice one colour, packed slice after slice: the pointer layout of a
// one-level array.
Vector<std::uint32_t> solidSlices(int slices, int texelsPerSlice)
{
    auto values = Vector<std::uint32_t> {};

    for (auto slice = 0; slice < slices; ++slice)
        for (auto index = 0; index < texelsPerSlice; ++index)
            values.add(sliceColor(slice));

    return values;
}

// Every level of every slice, slice-major, each level's texels distinct.
Vector<std::uint32_t> packedChains(int slices, int size, int levels)
{
    auto values = Vector<std::uint32_t> {};

    for (auto slice = 0; slice < slices; ++slice)
        for (auto level = 0; level < levels; ++level)
        {
            const auto count = extent(size, level) * extent(size, level);

            for (const auto value: levelTexels(slice, level, count))
                values.add(value);
        }

    return values;
}

TextureDescriptor describe(TextureType kind, int size, int layers = 1, int depth = 1)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.kind = kind;
    descriptor.arrayLength = layers;
    descriptor.volumeDepth = depth;
    return descriptor;
}

bool canTest()
{
    return Device::shared().isValid();
}
} // namespace

auto tArrayLimits = test("TextureArray/theDeviceReportsItsLimits") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
    {
        check(device.maxTextureLayers() == 0);
        check(device.maxTextureSize3D() == 0);
        return;
    }

    check(device.maxTextureLayers() >= 256);
    check(device.maxTextureSize3D() >= 256);

    if constexpr (Platform::isMac())
        check(device.supportsCubeArrays());
};

auto tArrayLayers = test("TextureArray/eachLayerSamplesItsOwnPixels") = []
{
    if (!canTest())
        return;

    constexpr auto layers = 4;
    const auto pixels = solidSlices(layers, 4);
    auto image = Device::shared().makeTexture(
        describe(TextureType::TwoDArray, 2, layers), pixels.data());

    check(image.isValid());
    check(image.kind() == TextureType::TwoDArray);
    check(image.arrayLength() == layers && image.slices() == layers);
    check(!image.isCube());

    if (!image.isValid())
        return;

    const auto columns = drawColumns(arrayDialects(), image, layers);

    for (auto layer = 0; layer < layers; ++layer)
        check(columns[(std::size_t) layer] == sliceColor(layer));
};

auto tVolumeSamples = test("TextureArray/aVolumeSamplesAlongZ") = []
{
    if (!canTest())
        return;

    constexpr auto depth = 4;
    const auto pixels = solidSlices(depth, 4);
    auto image = Device::shared().makeTexture(
        describe(TextureType::ThreeD, 2, 1, depth), pixels.data());

    check(image.isValid());
    check(image.kind() == TextureType::ThreeD);
    check(image.volumeDepth() == depth && image.slices() == 1);

    if (!image.isValid())
        return;

    const auto columns = drawColumns(volumeDialects(depth), image, depth);

    for (auto z = 0; z < depth; ++z)
        check(columns[(std::size_t) z] == sliceColor(z));
};

auto tCubeArray = test("TextureArray/aCubeArraySamplesEachCubeAndFace") = []
{
    if (!canTest() || !Device::shared().supportsCubeArrays())
        return;

    constexpr auto cubes = 2;
    const auto pixels = solidSlices(cubes * 6, 4);
    auto image = Device::shared().makeTexture(
        describe(TextureType::CubeArray, 2, cubes), pixels.data());

    check(image.isValid());
    check(image.slices() == cubes * 6 && image.arrayLength() == cubes);

    if (!image.isValid())
        return;

    const auto columns = drawColumns(cubeArrayDialects(), image, cubes * 6);

    for (auto slice = 0; slice < cubes * 6; ++slice)
        check(columns[(std::size_t) slice] == sliceColor(slice));
};

auto tCubeChain = test("TextureArray/aCubeTakesTheCallersChain") = []
{
    if (!canTest())
        return;

    constexpr auto size = 4;
    constexpr auto levels = 3;
    const auto pixels = packedChains(6, size, levels);

    auto descriptor = describe(TextureType::Cube, size);
    descriptor.mipLevels = levels;

    auto cube = Device::shared().makeTexture(descriptor, pixels.data());

    check(cube.isValid() && cube.isCube());
    check(cube.mipLevels() == levels);

    if (!cube.isValid())
        return;

    for (auto face = 0; face < 6; ++face)
        for (auto level = 0; level < levels; ++level)
        {
            const auto count = extent(size, level) * extent(size, level);
            check(readLevel(cube, level, face) == levelTexels(face, level, count));
        }
};

auto tLegacyCubeFlag = test("TextureArray/theCubeFlagIsTheCubeType") = []
{
    if (!canTest())
        return;

    static const auto pixels = solidSlices(6, 1);

    auto flag = TextureDescriptor {};
    flag.width = 1;
    flag.height = 1;
    flag.cube = true;

    auto cube = Device::shared().makeTexture(flag, pixels.data());

    check(cube.isCube() && cube.kind() == TextureType::Cube && cube.slices() == 6);

    auto contradiction = flag;
    contradiction.kind = TextureType::TwoDArray;

    check(!Device::shared().makeTexture(contradiction, pixels.data()).isValid());
};

auto tSubresources = test("TextureArray/subresourcesFillEveryLevelOfEveryLayer") = []
{
    if (!canTest())
        return;

    constexpr auto size = 8;
    constexpr auto layers = 3;
    constexpr auto levels = 3;

    auto descriptor = describe(TextureType::TwoDArray, size, layers);
    descriptor.mipLevels = levels;

    auto storage = Vector<Vector<std::uint32_t>> {};
    auto subresources = Vector<TextureSubresource> {};

    for (auto layer = 0; layer < layers; ++layer)
        for (auto level = 0; level < levels; ++level)
            storage.add(levelTexels(
                layer, level, extent(size, level) * extent(size, level)));

    for (auto layer = 0; layer < layers; ++layer)
        for (auto level = 0; level < levels; ++level)
            subresources.add(
                {level,
                 layer,
                 storage[(std::size_t) (layer * levels + level)].data()});

    auto texture = Device::shared().makeTextureFromSubresources(
        descriptor,
        std::span<const TextureSubresource> {subresources.data(),
                                             (std::size_t) subresources.size()});

    check(texture.isValid());

    if (!texture.isValid())
        return;

    for (auto layer = 0; layer < layers; ++layer)
        for (auto level = 0; level < levels; ++level)
            check(readLevel(texture, level, layer)
                  == storage[(std::size_t) (layer * levels + level)]);
};

auto tBuiltChains =
    test("TextureArray/mipmappedSubresourcesBuildEachLayersChain") = []
{
    if (!canTest())
        return;

    constexpr auto layers = 2;

    auto descriptor = describe(TextureType::TwoDArray, 2, layers);
    descriptor.mipmapped = true;

    const auto first = filled(4, 0xff0000ffu);
    const auto second = filled(4, 0xff00ff00u);

    const TextureSubresource subresources[] = {{0, 0, first.data()},
                                               {0, 1, second.data()}};

    auto texture =
        Device::shared().makeTextureFromSubresources(descriptor, subresources);

    check(texture.isValid() && texture.mipLevels() == 2);

    if (!texture.isValid())
        return;

    check(readLevel(texture, 1, 0) == filled(1, 0xff0000ffu));
    check(readLevel(texture, 1, 1) == filled(1, 0xff00ff00u));
};

auto tUpdateOneLevel = test("TextureArray/updateReachesLevelTwoOfLayerThree") = []
{
    if (!canTest())
        return;

    constexpr auto size = 8;
    constexpr auto layers = 4;
    constexpr auto levels = 3;
    const auto pixels = packedChains(layers, size, levels);

    auto descriptor = describe(TextureType::TwoDArray, size, layers);
    descriptor.mipLevels = levels;

    auto texture = Device::shared().makeTexture(descriptor, pixels.data());

    check(texture.isValid());

    if (!texture.isValid())
        return;

    // Level 2 is 2x2: replace its right column, from a source with a stride.
    const std::uint32_t replacement[] = {0xffabcdefu, 0, 0xff123456u, 0};

    auto region = TextureRegion {};
    region.level = 2;
    region.slice = 3;
    region.origin = {1, 0, 0};
    region.size = {1, 2, 1};
    region.bytesPerRow = 8;

    texture.updateSubresource(region, replacement);

    auto expected = levelTexels(3, 2, 4);
    expected[1] = 0xffabcdefu;
    expected[3] = 0xff123456u;

    check(readLevel(texture, 2, 3) == expected);
    check(readLevel(texture, 2, 2) == levelTexels(2, 2, 4));
    check(readLevel(texture, 1, 3) == levelTexels(3, 1, 16));

    // Outside the level, a level it lacks, a slice it lacks: all no-ops.
    auto outside = region;
    outside.origin = {2, 0, 0};
    texture.updateSubresource(outside, replacement);

    auto noLevel = region;
    noLevel.level = levels;
    texture.updateSubresource(noLevel, replacement);

    auto noSlice = region;
    noSlice.slice = layers;
    texture.updateSubresource(noSlice, replacement);

    check(readLevel(texture, 2, 3) == expected);
};

auto tVolumeBox = test("TextureArray/aVolumeUpdatesAndReadsABoxOfImages") = []
{
    if (!canTest())
        return;

    constexpr auto size = 4;
    const auto pixels = solidSlices(size, size * size);

    auto texture = Device::shared().makeTexture(
        describe(TextureType::ThreeD, size, 1, size), pixels.data());

    check(texture.isValid());

    if (!texture.isValid())
        return;

    const auto box = filled(2 * 2 * 2, 0xff777777u);

    auto region = TextureRegion {};
    region.origin = {1, 1, 1};
    region.size = {2, 2, 2};
    texture.updateSubresource(region, box.data());

    const auto all = readLevel(texture, 0, 0);
    auto mismatches = 0;

    for (auto z = 0; z < size; ++z)
        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
            {
                const auto inBox =
                    x >= 1 && x < 3 && y >= 1 && y < 3 && z >= 1 && z < 3;
                const auto expected = inBox ? 0xff777777u : sliceColor(z);

                if (all[(std::size_t) ((z * size + y) * size + x)] != expected)
                    ++mismatches;
            }

    check(mismatches == 0);

    auto back = Vector<std::uint32_t> {};
    back.resize(8);
    texture.readSubresource(region, back.data());

    check(back == box);
};

auto tTargetUpload = test("TextureArray/aRenderTargetArrayTakesAnUpload") = []
{
    if (!canTest())
        return;

    auto descriptor = describe(TextureType::TwoDArray, 2, 2);
    descriptor.renderTarget = true;

    auto target = Device::shared().makeTexture(descriptor);

    check(target.isValid() && target.isRenderTarget());

    if (!target.isValid())
        return;

    const auto values = levelTexels(1, 0, 4);
    auto region = TextureRegion::wholeLevel(0, 1);
    target.updateSubresource(region, values.data());

    check(readLevel(target, 0, 1) == values);
};

auto tArrayRefusals = test("TextureArray/refusesWhatItCannotMean") = []
{
    if (!canTest())
        return;

    static const auto pixels = solidSlices(64, 16);

    const auto refused = [](TextureDescriptor descriptor)
    { return !Device::shared().makeTexture(descriptor, pixels.data()).isValid(); };

    auto layersOnTwoD = describe(TextureType::TwoD, 4, 2);
    check(refused(layersOnTwoD));

    auto depthOnArray = describe(TextureType::TwoDArray, 4, 2, 2);
    check(refused(depthOnArray));

    auto noLayers = describe(TextureType::TwoDArray, 4, 0);
    check(refused(noLayers));

    auto oblongCubes = describe(TextureType::CubeArray, 4, 2);
    oblongCubes.height = 2;
    check(refused(oblongCubes));

    auto volumeTargetWithDepth = describe(TextureType::ThreeD, 4, 1, 4);
    volumeTargetWithDepth.renderTarget = true;
    volumeTargetWithDepth.depth = true;
    check(!Device::shared().makeTexture(volumeTargetWithDepth).isValid());

    auto multisampledArray = describe(TextureType::TwoDArray, 4, 2);
    multisampledArray.renderTarget = true;
    multisampledArray.sampleCount = 4;
    check(!Device::shared().makeTexture(multisampledArray).isValid());

    const TextureSubresource missingLayer[] = {{0, 2, pixels.data()}};
    check(!Device::shared()
               .makeTextureFromSubresources(describe(TextureType::TwoDArray, 4, 2),
                                            missingLayer)
               .isValid());

    check(!refused(describe(TextureType::TwoDArray, 4, 2)));
};
