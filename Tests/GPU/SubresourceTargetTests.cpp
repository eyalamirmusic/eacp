#include "Common.h"

#include <functional>

// Rendering into, dispatching at and copying between any level and slice:
// ColorTarget::level, slice and layerCount, ComputePass's level binds, and
// copies of a volume's images or of a run of layers. Every image is cleared to
// a colour of its own first, so a pass or a copy that lands one level, face,
// layer or image off reads back as the wrong colour rather than as a
// plausible one. The shaders are hand-written, the way bgfx's are.
//
// Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace SubresourceTargetTests
{
namespace
{
struct ColorVertex
{
    float position[3];
    float color[4];
};

constexpr auto mslSource = R"(
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

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]])
{
    return in.color;
}
)";

constexpr auto hlslSource = R"(
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

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 1.0);
    o.color = input.color;
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}
)";

constexpr auto glslSource = R"(#version 450

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
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = color;
}
#endif
)";

// The layer is the instance: instance n lands in layer n of the pass's run,
// coloured red n + 1 fifths of the way up.
constexpr auto mslLayered = R"(
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
    uint layer [[render_target_array_index]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]],
                            uint instance [[instance_id]])
{
    VertexOut out;
    out.position = float4(in.position, 1.0);
    out.color = float4(float(instance + 1) * 0.2, 0.0, 0.0, 1.0);
    out.layer = instance;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]])
{
    return in.color;
}
)";

constexpr auto hlslLayered = R"(
struct VertexIn
{
    float3 position : TEXCOORD0;
    float4 color : TEXCOORD1;
    uint instance : SV_InstanceID;
};

struct VertexOut
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
    uint layer : SV_RenderTargetArrayIndex;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 1.0);
    o.color = float4(float(input.instance + 1) * 0.2, 0.0, 0.0, 1.0);
    o.layer = input.instance;
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return input.color;
}
)";

constexpr auto glslLayered = R"(#version 450
#extension GL_ARB_shader_viewport_layer_array : require

#ifdef EACP_VERTEX
layout(location = 0) in vec3 attr0;
layout(location = 1) in vec4 attr1;
layout(location = 0) out vec4 color;

void main()
{
    gl_Position = vec4(attr0, 1.0);
    color = vec4(float(gl_InstanceIndex + 1) * 0.2, 0.0, 0.0, 1.0);
    gl_Layer = gl_InstanceIndex;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = color;
}
#endif
)";

// A 2x2 box filter from one level into the next: texel (x, y) of the output
// is the mean of the four input texels under it.
constexpr auto mslDownscale = R"(
#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d<float, access::read> source [[texture(0)]],
                        texture2d<float, access::write> target [[texture(1)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= target.get_width() || gid.y >= target.get_height())
        return;

    uint2 at = gid * 2;
    float4 sum = source.read(at) + source.read(at + uint2(1, 0))
                 + source.read(at + uint2(0, 1)) + source.read(at + uint2(1, 1));
    target.write(sum * 0.25, gid);
}
)";

constexpr auto hlslDownscale = R"(
RWTexture2D<float4> source : register(u8);
RWTexture2D<float4> target : register(u9);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);

    if (gid.x >= width || gid.y >= height)
        return;

    uint2 at = gid.xy * 2;
    float4 sum = source[at] + source[at + uint2(1, 0)] + source[at + uint2(0, 1)]
                 + source[at + uint2(1, 1)];
    target[gid.xy] = sum * 0.25;
}
)";

constexpr auto glslDownscale = R"(#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 8) uniform sampler2D source;
layout(set = 0, binding = 9, rgba8) uniform writeonly image2D target;

void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(target);

    if (gid.x >= size.x || gid.y >= size.y)
        return;

    ivec2 at = gid * 2;
    vec4 sum = texelFetch(source, at, 0) + texelFetch(source, at + ivec2(1, 0), 0)
               + texelFetch(source, at + ivec2(0, 1), 0)
               + texelFetch(source, at + ivec2(1, 1), 0);
    imageStore(target, gid, sum * 0.25);
}
)";

static_assert(vulkanComputeTextureBinding(0) == 8);
static_assert(vulkanComputeTextureBinding(1) == 9);

bool canTest()
{
    return Device::shared().isValid();
}

RenderPipeline makePipeline(const ShaderSource& source, bool depth = false)
{
    auto library = Device::shared().makeShaderLibrary(source);

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.depth = depth;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
    descriptor.vertexLayout.attribute(VertexFormat::Float3, 0);
    descriptor.vertexLayout.attribute(VertexFormat::Float4, 12);
    descriptor.vertexLayout.stride = sizeof(ColorVertex);

    return Device::shared().makeRenderPipeline(descriptor);
}

RenderPipeline makeColorPipeline(bool depth = false)
{
    auto source = nativeShaderSource(mslSource, hlslSource, glslSource);
    source.withVertex("vertexMain").withFragment("fragmentMain");
    return makePipeline(source, depth);
}

RenderPipeline makeLayeredPipeline(bool depth = false)
{
    auto source = nativeShaderSource(mslLayered, hlslLayered, glslLayered);
    source.withVertex("vertexMain").withFragment("fragmentMain");
    return makePipeline(source, depth);
}

// A full-screen quad at depth z, every vertex the one colour.
Buffer makeQuad(std::uint32_t rgba, float z = 0.5f)
{
    const auto channel = [&](int shift)
    { return (float) ((rgba >> shift) & 0xffu) / 255.f; };

    const auto corner = [&](float x, float y)
    {
        return ColorVertex {{x, y, z},
                            {channel(0), channel(8), channel(16), channel(24)}};
    };

    const ColorVertex vertices[] = {corner(-1.f, -1.f),
                                    corner(1.f, -1.f),
                                    corner(-1.f, 1.f),
                                    corner(1.f, -1.f),
                                    corner(1.f, 1.f),
                                    corner(-1.f, 1.f)};

    return Device::shared().makeBuffer(vertices, sizeof(vertices));
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

// A distinct opaque RGBA8 colour per image, as the bytes read back hold it.
std::uint32_t imageColor(int image)
{
    return 0xff000000u | (std::uint32_t) ((image * 37 + 11) & 0xff)
           | ((std::uint32_t) ((image * 53 + 7) & 0xff) << 8);
}

Graphics::Color asColor(std::uint32_t rgba)
{
    const auto channel = [&](int shift)
    { return (float) ((rgba >> shift) & 0xffu) / 255.f; };

    return {channel(0), channel(8), channel(16), channel(24)};
}

int extent(int size, int level)
{
    return std::max(1, size >> level);
}

int imagesAt(const Texture& texture, int level)
{
    return texture.kind() == TextureType::ThreeD
               ? extent(texture.volumeDepth(), level)
               : texture.slices();
}

// Every level of every slice (z of a volume) cleared to its own colour, one
// pass each. The image index runs level-major.
int imageIndex(const Texture& texture, int level, int slice)
{
    auto index = 0;

    for (auto earlier = 0; earlier < level; ++earlier)
        index += imagesAt(texture, earlier);

    return index + slice;
}

void clearEveryImage(Frame& frame, const Texture& texture)
{
    for (auto level = 0; level < texture.mipLevels(); ++level)
        for (auto slice = 0; slice < imagesAt(texture, level); ++slice)
        {
            auto target = ColorTarget {&texture};
            target.clearColor =
                asColor(imageColor(imageIndex(texture, level, slice)));
            target.level = level;
            target.slice = slice;

            frame.beginMultiTargetPass(std::span<const ColorTarget> {&target, 1});
        }
}

Vector<std::uint32_t> readImage(const Texture& texture, int level, int slice)
{
    const auto volume = texture.kind() == TextureType::ThreeD;

    auto region = TextureRegion::wholeLevel(level, volume ? 0 : slice);

    if (volume)
    {
        region.origin.z = slice;
        region.size.depth = 1;
    }

    auto values = Vector<std::uint32_t> {};
    values.resize((std::size_t) (extent(texture.width(), level)
                                 * extent(texture.height(), level)));
    texture.readSubresource(region, values.data());
    return values;
}

bool isFilledWith(const Vector<std::uint32_t>& values, std::uint32_t expected)
{
    const auto close = [](std::uint32_t a, std::uint32_t b)
    {
        for (auto shift = 0; shift < 32; shift += 8)
        {
            const auto x = (int) ((a >> shift) & 0xffu);
            const auto y = (int) ((b >> shift) & 0xffu);

            if (x < y - 2 || x > y + 2)
                return false;
        }

        return true;
    };

    if (values.empty())
        return false;

    for (auto value: values)
        if (!close(value, expected))
            return false;

    return true;
}

// Every image but `drawn` still holds the colour clearEveryImage gave it.
bool othersUntouched(const Texture& texture, int drawnLevel, int drawnSlice)
{
    for (auto level = 0; level < texture.mipLevels(); ++level)
        for (auto slice = 0; slice < imagesAt(texture, level); ++slice)
        {
            if (level == drawnLevel && slice == drawnSlice)
                continue;

            const auto expected = imageColor(imageIndex(texture, level, slice));

            if (!isFilledWith(readImage(texture, level, slice), expected))
                return false;
        }

    return true;
}

TextureDescriptor describeTarget(
    TextureType kind, int size, int levels = 1, int layers = 1, int depth = 1)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.kind = kind;
    descriptor.arrayLength = layers;
    descriptor.volumeDepth = depth;
    descriptor.mipLevels = levels;
    descriptor.renderTarget = true;
    return descriptor;
}

constexpr auto drawnColor = 0xff2040c0u;
constexpr auto nearColor = 0xff0000ffu;
constexpr auto farColor = 0xff00ff00u;

// Draws the drawn colour into one image of `texture`, the pass reporting that
// level's size. With depth, a near quad then a far one, so only a working
// depth plane at that level and slice leaves the near colour on top.
void drawInto(const Texture& texture, int level, int slice, bool depth = false)
{
    auto pipeline = makeColorPipeline(depth);
    auto quad = makeQuad(drawnColor);
    auto nearQuad = makeQuad(nearColor, 0.2f);
    auto farQuad = makeQuad(farColor, 0.8f);
    auto passWidth = -1;

    check(pipeline.isValid());

    runFrame(
        [&](Frame& frame)
        {
            clearEveryImage(frame, texture);

            auto target = ColorTarget {&texture};
            target.clear = false;
            target.level = level;
            target.slice = slice;

            auto pass = frame.beginMultiTargetPass(
                std::span<const ColorTarget> {&target, 1});
            passWidth = pass.targetWidth();
            pass.setPipeline(pipeline);

            if (depth)
            {
                pass.setVertexBuffer(nearQuad);
                pass.draw(6);
                pass.setVertexBuffer(farQuad);
                pass.draw(6);
            }
            else
            {
                pass.setVertexBuffer(quad);
                pass.draw(6);
            }
        });

    check(passWidth == extent(texture.width(), level));
}
} // namespace

auto tSubresourceRenderLevel = test("Subresource/aPassRendersIntoOneLevel") = []
{
    if (!canTest())
        return;

    auto texture =
        Device::shared().makeTexture(describeTarget(TextureType::TwoD, 8, 3));

    check(texture.isValid() && texture.mipLevels() == 3);

    if (!texture.isValid())
        return;

    drawInto(texture, 1, 0);

    check(isFilledWith(readImage(texture, 1, 0), drawnColor));
    check(othersUntouched(texture, 1, 0));
};

auto tSubresourceRenderLevelDepth =
    test("Subresource/theDepthPlaneFollowsTheLevel") = []
{
    if (!canTest())
        return;

    auto descriptor = describeTarget(TextureType::TwoD, 8, 2);
    descriptor.depth = true;

    auto texture = Device::shared().makeTexture(descriptor);

    check(texture.isValid() && texture.hasDepth());

    if (!texture.isValid())
        return;

    drawInto(texture, 1, 0, true);

    check(isFilledWith(readImage(texture, 1, 0), nearColor));
    check(othersUntouched(texture, 1, 0));
};

auto tSubresourceRenderLayer = test("Subresource/aPassRendersIntoOneLayer") = []
{
    if (!canTest())
        return;

    auto texture = Device::shared().makeTexture(
        describeTarget(TextureType::TwoDArray, 4, 2, 3));

    check(texture.isValid());

    if (!texture.isValid())
        return;

    drawInto(texture, 0, 2);

    check(isFilledWith(readImage(texture, 0, 2), drawnColor));
    check(othersUntouched(texture, 0, 2));
};

// A cube's caller-sized chain filled by passes, face by face and level by
// level: the render-to-cube a prefiltered environment map is made of.
auto tSubresourceRenderCubeFace =
    test("Subresource/aPassRendersIntoACubeFaceAtALevel") = []
{
    if (!canTest())
        return;

    auto descriptor = describeTarget(TextureType::Cube, 8, 2);
    descriptor.depth = true;

    auto texture = Device::shared().makeTexture(descriptor);

    check(texture.isValid() && texture.isCube() && texture.mipLevels() == 2);

    if (!texture.isValid())
        return;

    drawInto(texture, 1, 4, true);

    check(isFilledWith(readImage(texture, 1, 4), nearColor));
    check(othersUntouched(texture, 1, 4));
};

auto tSubresourceRenderVolume =
    test("Subresource/aPassRendersIntoOneImageOfAVolume") = []
{
    if (!canTest())
        return;

    auto texture = Device::shared().makeTexture(
        describeTarget(TextureType::ThreeD, 4, 2, 1, 4));

    check(texture.isValid() && texture.isRenderTarget());

    if (!texture.isValid())
        return;

    drawInto(texture, 1, 1);

    check(isFilledWith(readImage(texture, 1, 1), drawnColor));
    check(othersUntouched(texture, 1, 1));

    auto withDepth = describeTarget(TextureType::ThreeD, 4, 1, 1, 4);
    withDepth.depth = true;

    check(!Device::shared().makeTexture(withDepth).isValid());
};

auto tSubresourceLayered = test("Subresource/theVertexShaderPicksTheLayer") = []
{
    if (!canTest() || !Device::shared().supportsLayeredRendering())
        return;

    constexpr auto layers = 4;

    auto descriptor = describeTarget(TextureType::TwoDArray, 4, 1, layers);
    descriptor.depth = true;

    auto texture = Device::shared().makeTexture(descriptor);
    auto pipeline = makeLayeredPipeline(true);
    auto quad = makeQuad(drawnColor);

    check(texture.isValid() && pipeline.isValid());

    if (!texture.isValid() || !pipeline.isValid())
        return;

    // All four layers in one pass, then the middle two again as a run of their
    // own, so the shader's index is seen counting from the run's first slice.
    runFrame(
        [&](Frame& frame)
        {
            auto all = ColorTarget {&texture};
            all.layerCount = layers;

            {
                auto pass = frame.beginMultiTargetPass(
                    std::span<const ColorTarget> {&all, 1});
                pass.setPipeline(pipeline);
                pass.setVertexBuffer(quad);
                pass.drawInstanced(6, layers);
            }

            auto middle = ColorTarget {&texture};
            middle.clear = false;
            middle.slice = 1;
            middle.layerCount = 2;

            auto pass = frame.beginMultiTargetPass(
                std::span<const ColorTarget> {&middle, 1});
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(quad);
            pass.drawInstanced(6, 1);
        });

    const auto red = [](int fifths)
    { return 0xff000000u | (std::uint32_t) (fifths * 51); };

    check(isFilledWith(readImage(texture, 0, 0), red(1)));
    check(isFilledWith(readImage(texture, 0, 1), red(1)));
    check(isFilledWith(readImage(texture, 0, 2), red(3)));
    check(isFilledWith(readImage(texture, 0, 3), red(4)));
};

auto tSubresourceRefusals =
    test("Subresource/aPassRefusesImagesTheTextureLacks") = []
{
    if (!canTest())
        return;

    auto array = Device::shared().makeTexture(
        describeTarget(TextureType::TwoDArray, 8, 2, 3));
    auto volume = Device::shared().makeTexture(
        describeTarget(TextureType::ThreeD, 4, 1, 1, 4));
    auto small = Device::shared().makeTexture(describeTarget(TextureType::TwoD, 4));

    check(array.isValid() && volume.isValid() && small.isValid());

    const auto refused = [](std::span<const ColorTarget> targets)
    {
        auto width = -1;
        runFrame([&](Frame& frame)
                 { width = frame.beginMultiTargetPass(targets).targetWidth(); });
        return width == 0;
    };

    const auto one = [&](ColorTarget target)
    { return refused(std::span<const ColorTarget> {&target, 1}); };

    auto noLevel = ColorTarget {&array};
    noLevel.level = 2;
    check(one(noLevel));

    auto noSlice = ColorTarget {&array};
    noSlice.slice = 3;
    check(one(noSlice));

    auto longRun = ColorTarget {&array};
    longRun.slice = 1;
    longRun.layerCount = 3;
    check(one(longRun));

    auto volumeRun = ColorTarget {&volume};
    volumeRun.slice = 1;
    volumeRun.layerCount = 2;
    check(one(volumeRun));

    auto deepImage = ColorTarget {&volume};
    deepImage.slice = 4;
    check(one(deepImage));

    // Level 1 of the 8-wide array is the 4-wide target's size; level 0 is not.
    ColorTarget matching[] = {{&array}, {&small}};
    matching[0].level = 1;
    check(!refused(matching));

    matching[0].level = 0;
    check(refused(matching));
};

auto tSubresourceComputeLevels =
    test("Subresource/aKernelReadsOneLevelAndWritesTheNext") = []
{
    if (!canTest())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = 4;
    descriptor.height = 4;
    descriptor.computeWrite = true;
    descriptor.mipLevels = 3;

    auto texture = Device::shared().makeTexture(descriptor);

    check(texture.isValid() && texture.mipLevels() == 3);

    if (!texture.isValid())
        return;

    // Each 2x2 block of level 0 averages to a value of its own: red 40, 80,
    // 120 and 160 down the four blocks, so level 1 is those four and level 2
    // their mean, 100.
    auto levelZero = Vector<std::uint32_t> {};

    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 4; ++x)
        {
            const auto block = (y / 2) * 2 + (x / 2);
            const auto offset = ((x + y) % 2 == 0) ? 8 : -8;
            levelZero.add(0xff000000u | (std::uint32_t) (40 * (block + 1) + offset));
        }

    texture.updateSubresource(TextureRegion::wholeLevel(0), levelZero.data());

    auto source =
        nativeComputeShaderSource(mslDownscale, hlslDownscale, glslDownscale)
            .withCompute("computeMain");
    auto library = Device::shared().makeShaderLibrary(source);
    auto pipeline = Device::shared().makeComputePipeline(library);

    check(pipeline.isValid());

    runFrame(
        [&](Frame& frame)
        {
            for (auto level = 1; level < 3; ++level)
            {
                auto pass = frame.beginCompute();
                pass.setPipeline(pipeline);
                // D3D12 tracks a texture's state whole, so the level below is
                // read through a second write view there.
                if constexpr (Platform::isWindows())
                    pass.setOutputTextureLevel(texture, 0, level - 1);
                else
                    pass.setInputTextureLevel(texture, 0, level - 1);

                pass.setOutputTextureLevel(texture, 1, level);
                pass.dispatch(extent(4, level), extent(4, level));
            }
        });

    const auto levelOne = readImage(texture, 1, 0);
    const std::uint32_t expectedOne[] = {
        0xff000000u | 40, 0xff000000u | 80, 0xff000000u | 120, 0xff000000u | 160};

    check(levelOne.size() == 4);

    for (auto index = 0; index < 4 && index < (int) levelOne.size(); ++index)
        check(isFilledWith(Vector<std::uint32_t> {levelOne[(std::size_t) index]},
                           expectedOne[index]));

    check(isFilledWith(readImage(texture, 2, 0), 0xff000000u | 100));
    check(readImage(texture, 0, 0) == levelZero);
};

// A box of one volume's images into another's, z offsets on both sides, and
// the whole-texture copy reaching every image of every level.
auto tSubresourceVolumeCopy = test("Subresource/aCopyMovesABoxOfImages") = []
{
    if (!canTest())
        return;

    constexpr auto size = 4;

    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.kind = TextureType::ThreeD;
    descriptor.volumeDepth = size;

    auto pixels = Vector<std::uint32_t> {};

    for (auto index = 0; index < size * size * size; ++index)
        pixels.add(0xff000000u | (std::uint32_t) index);

    const auto zeros = Vector<std::uint32_t>(pixels.size());

    auto source = Device::shared().makeTexture(descriptor, pixels.data());
    auto boxed = Device::shared().makeTexture(descriptor, zeros.data());
    auto whole = Device::shared().makeTexture(descriptor, zeros.data());

    check(source.isValid() && boxed.isValid() && whole.isValid());

    if (!source.isValid())
        return;

    auto region = TextureCopyRegion {};
    region.sourceOrigin = {1, 1, 1};
    region.destinationOrigin = {0, 0, 2};
    region.size = {2, 2, 2};

    runFrame(
        [&](Frame& frame)
        {
            frame.copy(source, boxed, region);
            frame.copy(source, whole);
        });

    auto all = Vector<std::uint32_t> {};
    all.resize(pixels.size());
    boxed.readSubresource(TextureRegion::wholeLevel(0), all.data());

    auto mismatches = 0;

    for (auto z = 0; z < size; ++z)
        for (auto y = 0; y < size; ++y)
            for (auto x = 0; x < size; ++x)
            {
                const auto inBox = x < 2 && y < 2 && z >= 2;
                const auto from = ((z - 1) * size + (y + 1)) * size + (x + 1);
                const auto expected = inBox ? pixels[(std::size_t) from] : 0u;

                if (all[(std::size_t) ((z * size + y) * size + x)] != expected)
                    ++mismatches;
            }

    check(mismatches == 0);

    whole.readSubresource(TextureRegion::wholeLevel(0), all.data());
    check(all == pixels);
};

auto tSubresourceSliceRangeCopy = test("Subresource/aCopyMovesARunOfLayers") = []
{
    if (!canTest())
        return;

    constexpr auto layers = 4;

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;
    descriptor.kind = TextureType::TwoDArray;
    descriptor.arrayLength = layers;

    auto pixels = Vector<std::uint32_t> {};

    for (auto layer = 0; layer < layers; ++layer)
        for (auto texel = 0; texel < 4; ++texel)
            pixels.add(imageColor(layer) + (std::uint32_t) texel);

    const auto zeros = Vector<std::uint32_t>(pixels.size());

    auto source = Device::shared().makeTexture(descriptor, pixels.data());
    auto destination = Device::shared().makeTexture(descriptor, zeros.data());

    auto volumeDescriptor = descriptor;
    volumeDescriptor.kind = TextureType::ThreeD;
    volumeDescriptor.arrayLength = 1;
    volumeDescriptor.volumeDepth = layers;

    auto volume = Device::shared().makeTexture(volumeDescriptor, pixels.data());

    check(source.isValid() && destination.isValid() && volume.isValid());

    if (!source.isValid())
        return;

    auto region = TextureCopyRegion {};
    region.sourceSlice = 1;
    region.destinationSlice = 2;
    region.sliceCount = 2;

    auto pastTheEnd = region;
    pastTheEnd.sliceCount = 3;

    runFrame(
        [&](Frame& frame)
        {
            frame.copy(source, destination, region);
            frame.copy(source, destination, pastTheEnd);
            frame.copy(volume, destination, TextureCopyRegion {});
        });

    const auto layer = [&](int index)
    {
        auto values = Vector<std::uint32_t> {};
        values.resize(4);
        destination.readSubresource(TextureRegion::wholeLevel(0, index),
                                    values.data());
        return values;
    };

    const auto sourceLayer = [&](int index)
    {
        auto values = Vector<std::uint32_t> {};

        for (auto texel = 0; texel < 4; ++texel)
            values.add(pixels[(std::size_t) (index * 4 + texel)]);

        return values;
    };

    check(layer(0) == Vector<std::uint32_t>(4));
    check(layer(1) == Vector<std::uint32_t>(4));
    check(layer(2) == sourceLayer(1));
    check(layer(3) == sourceLayer(2));
};
} // namespace SubresourceTargetTests
