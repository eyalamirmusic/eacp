#include "Common.h"

#include <eacp/GPU/Codegen/PackedScalars.h>
#include <eacp/GPU/Texture/MipChain.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>

// The rest of the format table: the sRGB twins of RGBA8, BGRA8 and the four
// older BC formats, BC4, BC5 and BC6H, RG16Unorm and RGBA16Unorm, the packed
// RGB10A2Unorm and RG11B10Float, the shared-exponent RGB9E5Float, and the 8-,
// 16- and 32-bit Uint and Sint formats. Each is created, uploaded and read back byte for byte; sampled (or
// read, for the integer ones) by a draw whose answer lands in a 32-bit target
// and is compared with a decode written down here; rendered into and read
// back where it is renderable; written by a kernel where the device says it
// can be; and given a mip chain, on the CPU and on the GPU, where it has one.
//
// The sRGB cases are the numerical ones: a sample is decoded, before the
// filter rather than after it, a pass's output is encoded, after blending in
// linear light, and a chain is averaged in light. The shaders are hand-written,
// the way bgfx's are.
//
// Self-skips without a device; the block cases also on one without BC formats.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto targetSize = 4;
constexpr auto texelCount = targetSize * targetSize;

bool canTest()
{
    return Device::shared().isValid();
}

struct QuadVertex
{
    float position[2];
    float uv[2];
};

constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
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

void runFrame(std::function<void(Frame&)> draw)
{
    auto view = FrameRunner {};
    view.draw = std::move(draw);
    view.setBounds({0.f, 0.f, 4.f, 4.f});
    view.renderToImage(1.f);
}

// ---------------------------------------------------------------------------
// Shaders. The quad's uv runs 0..1 across the target; the integer readers
// address texels by the fragment's own position instead.
// ---------------------------------------------------------------------------

constexpr auto mslSample = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.uv = in.uv;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             texture2d<float> image [[texture(0)]],
                             sampler imageSampler [[sampler(0)]])
{
    return image.sample(imageSampler, in.uv);
}
)";

// The static sampler a TextureSampling picks is s<samplingIndex>: 0 for
// Nearest and Clamp, 2 for Linear and Clamp.
std::string hlslSample(int samplerRegister)
{
    return R"(
Texture2D image : register(t0);
SamplerState imageSampler : register(s)"
           + std::to_string(samplerRegister) + R"();

struct VertexIn
{
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOut vertexMain(VertexIn input)
{
    VertexOut o;
    o.position = float4(input.position, 0.0, 1.0);
    o.uv = input.uv;
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target
{
    return image.Sample(imageSampler, input.uv);
}
)";
}

constexpr auto glslSample = R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec2 attr1;
layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
    uv = attr1;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 uv;
layout(set = 0, binding = 8) uniform sampler2D image;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = texture(image, uv);
}
#endif
)";

// The integer readers: texel (x, y) of the source into pixel (x, y) of an
// RGBA32Uint or RGBA32Sint target, every channel, missing ones included.
std::string mslReadIntegers(const char* scalar)
{
    const auto type = std::string {scalar};

    return R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    return out;
}

fragment )" + type
           + R"(4 fragmentMain(VertexOut in [[stage_in]],
                            texture2d<)"
           + type + R"(> image [[texture(0)]])
{
    return image.read(uint2(in.position.xy));
}
)";
}

std::string hlslReadIntegers(const char* scalar)
{
    const auto type = std::string {scalar};

    return R"(
Texture2D<)"
           + type + R"(4> image : register(t0);

float4 vertexMain(float2 position : TEXCOORD0, float2 uv : TEXCOORD1) : SV_Position
{
    return float4(position, 0.0, 1.0);
}

)" + type + R"(4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    return image.Load(int3(int2(position.xy), 0));
}
)";
}

std::string glslReadIntegers(const char* prefix)
{
    const auto p = std::string {prefix};

    return R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec2 attr1;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(set = 0, binding = 8) uniform )"
           + p + R"(sampler2D image;
layout(location = 0) out )"
           + p + R"(vec4 fragColor;

void main()
{
    fragColor = texelFetch(image, ivec2(gl_FragCoord.xy), 0);
}
#endif
)";
}

// A flat colour, for the passes that render into a format: (0.25, 0.5, 0.75,
// 1) as floats, (7, 100, 200, 250) as uints and (-7, 100, -100, 120) as ints -
// numbers every width of every format holds.
std::string mslFlat(const char* type, const char* value)
{
    return std::string {R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    return out;
}

fragment )"}
           + type + R"( fragmentMain(VertexOut in [[stage_in]])
{
    return )"
           + value + R"(;
}
)";
}

std::string hlslFlat(const char* type, const char* value)
{
    return std::string {R"(
float4 vertexMain(float2 position : TEXCOORD0, float2 uv : TEXCOORD1) : SV_Position
{
    return float4(position, 0.0, 1.0);
}

)"} + type + R"( fragmentMain(float4 position : SV_Position) : SV_Target
{
    return )"
           + value + R"(;
}
)";
}

std::string glslFlat(const char* type, const char* value)
{
    return std::string {R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in vec2 attr1;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) out )"}
           + type + R"( fragColor;

void main()
{
    fragColor = )"
           + value + R"(;
}
#endif
)";
}

constexpr float flatColor[] = {0.25f, 0.5f, 0.75f, 1.f};
constexpr std::int64_t flatUints[] = {7, 100, 200, 250};
constexpr std::int64_t flatInts[] = {-7, 100, -100, 120};

ShaderLibrary makeLibrary(
    const std::string& msl,
    const std::string& hlsl,
    const std::string& glsl,
    const std::source_location& location = std::source_location::current())
{
    auto source = nativeShaderSource(msl, hlsl, glsl, location);
    return Device::shared().makeShaderLibrary(
        source.withVertex("vertexMain").withFragment("fragmentMain"));
}

RenderPipeline makePipeline(const ShaderLibrary& library,
                            PixelFormat format,
                            BlendMode blend = BlendMode::None)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.colorFormat = format;
    descriptor.blendMode = blend;
    descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
    descriptor.vertexLayout.attribute(VertexFormat::Float2, 8);
    descriptor.vertexLayout.stride = sizeof(QuadVertex);

    return Device::shared().makeRenderPipeline(descriptor);
}

Texture makeTexture(TextureFormat format,
                    int width,
                    int height,
                    const void* pixels,
                    bool renderTarget = false)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = format;
    descriptor.renderTarget = renderTarget;

    return Device::shared().makeTexture(descriptor, pixels);
}

// Draws `source` into a fresh target of `targetFormat` through `pipeline`, and
// hands back the target's bytes.
Vector<std::uint8_t> drawInto(const RenderPipeline& pipeline,
                              TextureFormat targetFormat,
                              int width,
                              int height,
                              const Texture* source,
                              TextureSampling sampling = {},
                              int draws = 1)
{
    auto target = makeTexture(targetFormat, width, height, nullptr, true);
    auto quad = Device::shared().makeBuffer(fullQuad, sizeof(fullQuad));

    auto bytes = Vector<std::uint8_t> {};

    if (!target.isValid() || !pipeline.isValid())
        return bytes;

    runFrame(
        [&](Frame& frame)
        {
            auto pass = frame.beginPass(target, {{0.f, 0.f, 0.f, 0.f}});
            pass.setPipeline(pipeline);
            pass.setVertexBuffer(quad);

            if (source != nullptr)
                pass.setFragmentTexture(*source, 0, sampling);

            for (auto draw = 0; draw < draws; ++draw)
                pass.draw(6);
        });

    bytes.resize((std::size_t) (width * height * bytesPerPixel(targetFormat)));
    target.read(bytes.data());
    return bytes;
}

float floatAt(const Vector<std::uint8_t>& bytes, int texel, int channel)
{
    auto value = 0.f;
    std::memcpy(&value, bytes.data() + (texel * 4 + channel) * 4, sizeof(value));
    return value;
}

bool near(double value, double expected, double tolerance)
{
    return std::fabs(value - expected) <= tolerance;
}

double srgbToLinear(double encoded)
{
    return encoded <= 0.04045 ? encoded / 12.92
                              : std::pow((encoded + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double linear)
{
    return linear <= 0.0031308 ? linear * 12.92
                               : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

// RG11B10Float's channels: a half's exponent and bias, no sign, six or five
// bits of mantissa.
double smallFloat(std::uint32_t bits, int mantissa)
{
    return halfToFloat((std::uint16_t) (bits << (10 - mantissa)));
}

std::uint32_t packSmallFloat(float value, int mantissa)
{
    return (std::uint32_t) halfFromFloat(value) >> (10 - mantissa);
}

std::uint32_t packRG11B10(float r, float g, float b)
{
    return packSmallFloat(r, 6) | (packSmallFloat(g, 6) << 11)
           | (packSmallFloat(b, 5) << 22);
}

std::uint32_t
    packRGB10A2(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a)
{
    return r | (g << 10) | (b << 20) | (a << 30);
}

// Channel `channel` of one texel of a float or normalised format, decoded.
// The sRGB ones come back as their stored code value over 255: the encode is
// what the cases using this check.
double channelOf(TextureFormat format, const std::uint8_t* texel, int channel)
{
    auto word = std::uint32_t {};
    std::memcpy(&word, texel, sizeof(word));

    auto bits = std::uint16_t {};

    switch (format)
    {
        case TextureFormat::RGBA8UnormSrgb:
        case TextureFormat::BGRA8UnormSrgb:
        case TextureFormat::RGBA8Unorm:
            return texel[channel] / 255.0;

        case TextureFormat::RG16Unorm:
        case TextureFormat::RGBA16Unorm:
            std::memcpy(&bits, texel + channel * 2, sizeof(bits));
            return bits / 65535.0;

        case TextureFormat::RGB10A2Unorm:
            if (channel == 3)
                return (word >> 30) / 3.0;
            return ((word >> (10 * channel)) & 0x3ffu) / 1023.0;

        case TextureFormat::RG11B10Float:
            if (channel == 2)
                return smallFloat(word >> 22, 5);
            return smallFloat((word >> (11 * channel)) & 0x7ffu, 6);

        default:
            return 0.0;
    }
}

// Channel `channel` of one texel of an integer format, sign-extended where
// the format is signed.
std::int64_t integerOf(TextureFormat format, const std::uint8_t* texel, int channel)
{
    const auto width = bytesPerPixel(format) / formatChannels(format);
    auto value = std::uint64_t {0};
    std::memcpy(&value, texel + channel * width, (std::size_t) width);

    if (!isSignedIntegerFormat(format))
        return (std::int64_t) value;

    const auto bits = width * 8;
    const auto sign = std::uint64_t {1} << (bits - 1);

    return (std::int64_t) ((value ^ sign)) - (std::int64_t) sign;
}

void storeInteger(TextureFormat format,
                  std::uint8_t* texel,
                  int channel,
                  std::int64_t value)
{
    const auto width = bytesPerPixel(format) / formatChannels(format);
    const auto bits = (std::uint64_t) value;
    std::memcpy(texel + channel * width, &bits, (std::size_t) width);
}

// One value per channel that uses the top bit of its width: what a format
// read at the wrong width or sign gets wrong.
std::int64_t integerSeed(TextureFormat format, int channel)
{
    const auto bits = 8 * bytesPerPixel(format) / formatChannels(format);
    const auto top = std::int64_t {1} << (bits - 1);

    return isSignedIntegerFormat(format) ? -(top - 3) + channel : top + 5 + channel;
}

constexpr TextureFormat lastFormat = TextureFormat::BC6HRGBUfloat;

constexpr TextureFormat newUncompressed[] = {
    TextureFormat::RGBA8UnormSrgb, TextureFormat::BGRA8UnormSrgb,
    TextureFormat::RG16Unorm,      TextureFormat::RGBA16Unorm,
    TextureFormat::RGB10A2Unorm,   TextureFormat::RG11B10Float,
    TextureFormat::R8Uint,         TextureFormat::RG8Uint,
    TextureFormat::RGBA8Uint,      TextureFormat::R16Uint,
    TextureFormat::RG16Uint,       TextureFormat::RGBA16Uint,
    TextureFormat::RG32Uint,       TextureFormat::RGBA32Uint,
    TextureFormat::R8Sint,         TextureFormat::RG8Sint,
    TextureFormat::RGBA8Sint,      TextureFormat::R16Sint,
    TextureFormat::RG16Sint,       TextureFormat::RGBA16Sint,
    TextureFormat::R32Sint,        TextureFormat::RG32Sint,
    TextureFormat::RGBA32Sint,
};

constexpr TextureFormat integerFormats[] = {
    TextureFormat::R8Uint,
    TextureFormat::RG8Uint,
    TextureFormat::RGBA8Uint,
    TextureFormat::R16Uint,
    TextureFormat::RG16Uint,
    TextureFormat::RGBA16Uint,
    TextureFormat::R32Uint,
    TextureFormat::RG32Uint,
    TextureFormat::RGBA32Uint,
    TextureFormat::R8Sint,
    TextureFormat::RG8Sint,
    TextureFormat::RGBA8Sint,
    TextureFormat::R16Sint,
    TextureFormat::RG16Sint,
    TextureFormat::RGBA16Sint,
    TextureFormat::R32Sint,
    TextureFormat::RG32Sint,
    TextureFormat::RGBA32Sint,
};

// The new formats a pass writes floats into, with what each reads back as.
constexpr TextureFormat floatTargets[] = {
    TextureFormat::RGBA8UnormSrgb,
    TextureFormat::BGRA8UnormSrgb,
    TextureFormat::RG16Unorm,
    TextureFormat::RGBA16Unorm,
    TextureFormat::RGB10A2Unorm,
    TextureFormat::RG11B10Float,
};

// What a stored channel of floatTargets holds for an input of `value`: the
// value itself, encoded on the sRGB ones, and a step's worth of tolerance.
double storedFor(TextureFormat format, double value)
{
    return isSrgbFormat(format) ? linearToSrgb(value) : value;
}

double stepOf(TextureFormat format, int channel)
{
    switch (format)
    {
        case TextureFormat::RG16Unorm:
        case TextureFormat::RGBA16Unorm:
            return 1.0 / 65535.0;
        case TextureFormat::RGB10A2Unorm:
            return channel == 3 ? 1.0 / 3.0 : 1.0 / 1023.0;
        case TextureFormat::RG11B10Float:
            return 1.0 / 64.0;
        default:
            return 1.0 / 255.0;
    }
}

// Where in a BGRA texel the red, green, blue and alpha bytes are.
int storedChannel(TextureFormat format, int channel)
{
    if (format == TextureFormat::BGRA8UnormSrgb && channel < 3)
        return 2 - channel;

    return channel;
}
} // namespace

auto tFormatTraits = test("Formats/everyFormatDescribesItself") = []
{
    check(bytesPerPixel(TextureFormat::RGBA8UnormSrgb) == 4);
    check(bytesPerPixel(TextureFormat::RG16Unorm) == 4);
    check(bytesPerPixel(TextureFormat::RGBA16Unorm) == 8);
    check(bytesPerPixel(TextureFormat::RGB10A2Unorm) == 4);
    check(bytesPerPixel(TextureFormat::RG11B10Float) == 4);
    check(bytesPerPixel(TextureFormat::R8Sint) == 1);
    check(bytesPerPixel(TextureFormat::RGBA16Uint) == 8);
    check(bytesPerPixel(TextureFormat::RGBA32Sint) == 16);
    check(bytesPerPixel(TextureFormat::RG32Uint) == 8);

    check(bytesPerBlock(TextureFormat::BC1RGBASrgb) == 8);
    check(bytesPerBlock(TextureFormat::BC4RUnorm) == 8);
    check(bytesPerBlock(TextureFormat::BC4RSnorm) == 8);
    check(bytesPerBlock(TextureFormat::BC5RGUnorm) == 16);
    check(bytesPerBlock(TextureFormat::BC6HRGBUfloat) == 16);
    check(bytesPerBlock(TextureFormat::BC7RGBASrgb) == 16);
    check(bytesPerPixel(TextureFormat::BC5RGSnorm) == 0);

    check(isCompressedFormat(TextureFormat::BC6HRGBUfloat));
    check(!isCompressedFormat(TextureFormat::RG11B10Float));

    check(isFloatFormat(TextureFormat::RG11B10Float));
    check(isFloatFormat(TextureFormat::BC6HRGBUfloat));
    check(!isFloatFormat(TextureFormat::RGB10A2Unorm));

    check(isIntegerFormat(TextureFormat::R8Uint)
          && isIntegerFormat(TextureFormat::R8Sint));
    check(isUnsignedIntegerFormat(TextureFormat::RGBA16Uint));
    check(!isSignedIntegerFormat(TextureFormat::RGBA16Uint));
    check(isSignedIntegerFormat(TextureFormat::R32Sint));
    check(!isIntegerFormat(TextureFormat::RGBA8UnormSrgb));
    check(!canBuildMipChain(TextureFormat::RG8Sint));
    check(canBuildMipChain(TextureFormat::RGBA8UnormSrgb));

    check(isSrgbFormat(TextureFormat::BGRA8UnormSrgb));
    check(isSrgbFormat(TextureFormat::BC3RGBASrgb));
    check(!isSrgbFormat(TextureFormat::RGBA8Unorm));

    check(formatChannels(TextureFormat::RG11B10Float) == 3);
    check(formatChannels(TextureFormat::BC5RGSnorm) == 2);

    // Every format: a size of one kind or the other, a twin that comes back,
    // and an attachment format that names it again.
    for (auto value = 0; value <= (int) lastFormat; ++value)
    {
        const auto format = (TextureFormat) value;

        check((bytesPerPixel(format) > 0) != (bytesPerBlock(format) > 0));
        check(formatChannels(format) >= 1 && formatChannels(format) <= 4);
        check(linearFormatFor(srgbFormatFor(format)) == linearFormatFor(format));
        check(isSrgbFormat(srgbFormatFor(format))
                  == (srgbFormatFor(format) != format)
              || isSrgbFormat(format));
        check(!isSrgbFormat(linearFormatFor(format)));
        check(bytesPerBlock(srgbFormatFor(format)) == bytesPerBlock(format));

        if (isRenderableFormat(format))
            check(textureFormatFor(pixelFormatFor(format)) == format);
    }

    // RGB9E5Float: a 32-bit word of three channels, float, sampled only.
    check(bytesPerPixel(TextureFormat::RGB9E5Float) == 4);
    check(formatChannels(TextureFormat::RGB9E5Float) == 3);
    check(isFloatFormat(TextureFormat::RGB9E5Float));
    check(!isCompressedFormat(TextureFormat::RGB9E5Float));
    check(!isRenderableFormat(TextureFormat::RGB9E5Float));
    check(!isRenderableFormat(TextureFormat::BC1RGBA));
    check(isRenderableFormat(TextureFormat::RG11B10Float));
    check(isRenderableFormat(TextureFormat::RGBA32Sint));
    check(canBuildMipChain(TextureFormat::RGB9E5Float));
    check(srgbFormatFor(TextureFormat::RGB9E5Float) == TextureFormat::RGB9E5Float);

    check(srgbFormatFor(TextureFormat::BC7RGBA) == TextureFormat::BC7RGBASrgb);
    check(srgbFormatFor(TextureFormat::RG16Unorm) == TextureFormat::RG16Unorm);
    check(isIntegerFormat(PixelFormat::RGBA16Sint));
    check(!isIntegerFormat(PixelFormat::BGRA8UnormSrgb));
};

auto tFormatRoundTrip = test("Formats/everyNewFormatUploadsAndReadsBack") = []
{
    if (!canTest())
        return;

    for (const auto format: newUncompressed)
    {
        const auto stride = bytesPerPixel(format);
        auto bytes = Vector<std::uint8_t> {};

        for (auto index = 0; index < texelCount * stride; ++index)
            bytes.add((std::uint8_t) (index * 37 + 11));

        // RG11B10Float's bytes are all numbers: no exponent of 31, which some
        // copies would be free to quieten.
        if (format == TextureFormat::RG11B10Float)
            for (auto texel = 0; texel < texelCount; ++texel)
            {
                const auto word = packRG11B10(
                    0.125f * (float) texel, 2.f, 0.0625f * (float) texel);
                std::memcpy(bytes.data() + texel * 4, &word, 4);
            }

        for (const auto renderTarget: {false, true})
        {
            auto texture = makeTexture(format,
                                       targetSize,
                                       targetSize,
                                       renderTarget ? nullptr : bytes.data(),
                                       renderTarget);

            check(texture.isValid());
            check(texture.format() == format);

            if (!texture.isValid())
                continue;

            if (renderTarget)
                texture.update(bytes.data());

            auto back = Vector<std::uint8_t> {};
            back.resize(bytes.size());
            texture.read(back.data());

            check(std::memcmp(back.data(), bytes.data(), bytes.size()) == 0);
        }
    }
};

auto tFormatSrgbSample = test("Formats/srgbIsDecodedWhenSampled") = []
{
    if (!canTest())
        return;

    auto library = makeLibrary(mslSample, hlslSample(0), glslSample);
    auto pipeline = makePipeline(library, PixelFormat::RGBA32Float);

    constexpr std::uint8_t texel[] = {0, 128, 255, 64};

    for (const auto format:
         {TextureFormat::RGBA8UnormSrgb, TextureFormat::BGRA8UnormSrgb})
    {
        auto pixels = Vector<std::uint8_t> {};

        for (auto index = 0; index < texelCount; ++index)
            for (auto channel = 0; channel < 4; ++channel)
                pixels.add(texel[storedChannel(format, channel)]);

        auto source = makeTexture(format, targetSize, targetSize, pixels.data());
        check(source.isValid());

        const auto out = drawInto(
            pipeline, TextureFormat::RGBA32Float, targetSize, targetSize, &source);
        check(out.size() == (std::size_t) (texelCount * 16));

        if (out.empty())
            continue;

        for (auto index = 0; index < texelCount; ++index)
        {
            for (auto channel = 0; channel < 3; ++channel)
                check(near(floatAt(out, index, channel),
                           srgbToLinear(texel[channel] / 255.0),
                           1.5e-3));

            // Alpha is linear in an sRGB format.
            check(near(floatAt(out, index, 3), 64.0 / 255.0, 1.5e-3));
        }
    }
};

auto tFormatSrgbFilter = test("Formats/srgbIsDecodedBeforeTheFilter") = []
{
    if (!canTest())
        return;

    // Halfway between a black texel and a white one: 0.5 when the sampler
    // decodes each texel and then filters, which is the rule, and 0.214 when
    // it filters the code values and decodes the mean.
    auto library = makeLibrary(mslSample, hlslSample(2), glslSample);
    auto pipeline = makePipeline(library, PixelFormat::RGBA32Float);

    constexpr std::uint8_t pixels[] = {0, 0, 0, 255, 255, 255, 255, 255};
    auto source = makeTexture(TextureFormat::RGBA8UnormSrgb, 2, 1, pixels);

    const auto out = drawInto(pipeline,
                              TextureFormat::RGBA32Float,
                              1,
                              1,
                              &source,
                              {TextureFilter::Linear, TextureAddressMode::Clamp});

    check(out.size() == 16u);

    if (out.size() == 16u)
        for (auto channel = 0; channel < 3; ++channel)
            check(near(floatAt(out, 0, channel), 0.5, 4e-3));
};

auto tFormatPackedSample = test("Formats/sixteenBitAndPackedFormatsSample") = []
{
    if (!canTest())
        return;

    auto library = makeLibrary(mslSample, hlslSample(0), glslSample);
    auto pipeline = makePipeline(library, PixelFormat::RGBA32Float);

    struct Case
    {
        TextureFormat format;
        std::uint8_t texel[8];
        double expected[4];
    };

    const auto rgb10a2 = packRGB10A2(256, 512, 768, 2);
    const auto rg11b10 = packRG11B10(0.25f, 0.5f, 2.f);

    auto cases = Vector<Case> {};

    auto rg16 = Case {TextureFormat::RG16Unorm,
                      {0x00, 0x40, 0x00, 0xc0},
                      {16384.0 / 65535.0, 49152.0 / 65535.0, 0.0, 1.0}};
    cases.add(rg16);

    auto rgba16 = Case {TextureFormat::RGBA16Unorm,
                        {0x00, 0x40, 0x00, 0xc0, 0xff, 0xff, 0x00, 0x00},
                        {16384.0 / 65535.0, 49152.0 / 65535.0, 1.0, 0.0}};
    cases.add(rgba16);

    auto packed = Case {TextureFormat::RGB10A2Unorm,
                        {},
                        {256.0 / 1023.0, 512.0 / 1023.0, 768.0 / 1023.0, 2.0 / 3.0}};
    std::memcpy(packed.texel, &rgb10a2, 4);
    cases.add(packed);

    auto small = Case {TextureFormat::RG11B10Float, {}, {0.25, 0.5, 2.0, 1.0}};
    std::memcpy(small.texel, &rg11b10, 4);
    cases.add(small);

    for (const auto& item: cases)
    {
        const auto stride = bytesPerPixel(item.format);
        auto pixels = Vector<std::uint8_t> {};

        for (auto index = 0; index < texelCount; ++index)
            for (auto byte = 0; byte < stride; ++byte)
                pixels.add(item.texel[byte]);

        auto source =
            makeTexture(item.format, targetSize, targetSize, pixels.data());
        check(source.isValid());

        const auto out = drawInto(
            pipeline, TextureFormat::RGBA32Float, targetSize, targetSize, &source);
        check(out.size() == (std::size_t) (texelCount * 16));

        if (out.empty())
            continue;

        for (auto index = 0; index < texelCount; ++index)
            for (auto channel = 0; channel < 4; ++channel)
                check(near(
                    floatAt(out, index, channel), item.expected[channel], 1e-4));
    }
};

auto tFormatIntegerRead = test("Formats/integerFormatsReadAsIntegers") = []
{
    if (!canTest())
        return;

    auto uintLibrary = makeLibrary(
        mslReadIntegers("uint"), hlslReadIntegers("uint"), glslReadIntegers("u"));
    auto intLibrary = makeLibrary(
        mslReadIntegers("int"), hlslReadIntegers("int"), glslReadIntegers("i"));

    auto uintPipeline = makePipeline(uintLibrary, PixelFormat::RGBA32Uint);
    auto intPipeline = makePipeline(intLibrary, PixelFormat::RGBA32Sint);

    check(uintPipeline.isValid() && intPipeline.isValid());

    for (const auto format: integerFormats)
    {
        const auto stride = bytesPerPixel(format);
        const auto channels = formatChannels(format);
        const auto signedFormat = isSignedIntegerFormat(format);

        auto pixels = Vector<std::uint8_t> {};
        pixels.resize((std::size_t) (texelCount * stride));

        for (auto index = 0; index < texelCount; ++index)
            for (auto channel = 0; channel < channels; ++channel)
                storeInteger(format,
                             pixels.data() + index * stride,
                             channel,
                             integerSeed(format, channel) + index % 2);

        auto source = makeTexture(format, targetSize, targetSize, pixels.data());
        check(source.isValid());

        const auto targetFormat =
            signedFormat ? TextureFormat::RGBA32Sint : TextureFormat::RGBA32Uint;

        const auto out = drawInto(signedFormat ? intPipeline : uintPipeline,
                                  targetFormat,
                                  targetSize,
                                  targetSize,
                                  &source);

        check(out.size() == (std::size_t) (texelCount * 16));

        if (out.empty())
            continue;

        for (auto index = 0; index < texelCount; ++index)
            for (auto channel = 0; channel < 4; ++channel)
            {
                const auto got =
                    integerOf(targetFormat, out.data() + index * 16, channel);

                // A channel the format lacks reads 0, and alpha 1.
                const auto expected = channel < channels
                                          ? integerSeed(format, channel) + index % 2
                                          : (channel == 3 ? 1 : 0);

                check(got == expected);
            }
    }
};

auto tFormatFloatTargets = test("Formats/aPassDrawsIntoEachNewFloatFormat") = []
{
    if (!canTest())
        return;

    auto library = makeLibrary(mslFlat("float4", "float4(0.25, 0.5, 0.75, 1.0)"),
                               hlslFlat("float4", "float4(0.25, 0.5, 0.75, 1.0)"),
                               glslFlat("vec4", "vec4(0.25, 0.5, 0.75, 1.0)"));

    for (const auto format: floatTargets)
    {
        auto pipeline = makePipeline(library, pixelFormatFor(format));
        check(pipeline.isValid());

        const auto out = drawInto(pipeline, format, targetSize, targetSize, nullptr);
        const auto stride = bytesPerPixel(format);

        check(out.size() == (std::size_t) (texelCount * stride));

        if (out.empty())
            continue;

        for (auto index = 0; index < texelCount; ++index)
            for (auto channel = 0; channel < formatChannels(format); ++channel)
            {
                // Alpha is never encoded.
                const auto expected = channel == 3
                                          ? flatColor[3]
                                          : storedFor(format, flatColor[channel]);

                check(near(channelOf(format,
                                     out.data() + index * stride,
                                     storedChannel(format, channel)),
                           expected,
                           stepOf(format, channel) * 0.75));
            }
    }
};

auto tFormatSrgbBlend = test("Formats/srgbTargetsBlendInLinearLight") = []
{
    if (!canTest())
        return;

    // Two additive draws of 0.25 over a clear of 0: 0.5 in light, stored as
    // its encoding (188). Blending the stored code values instead would make
    // it 137 + 137.
    auto library = makeLibrary(mslFlat("float4", "float4(0.25, 0.25, 0.25, 1.0)"),
                               hlslFlat("float4", "float4(0.25, 0.25, 0.25, 1.0)"),
                               glslFlat("vec4", "vec4(0.25, 0.25, 0.25, 1.0)"));

    auto pipeline =
        makePipeline(library, PixelFormat::RGBA8UnormSrgb, BlendMode::Additive);

    const auto out = drawInto(pipeline,
                              TextureFormat::RGBA8UnormSrgb,
                              targetSize,
                              targetSize,
                              nullptr,
                              {},
                              2);

    check(out.size() == (std::size_t) (texelCount * 4));

    if (!out.empty())
        for (auto channel = 0; channel < 3; ++channel)
            check(std::abs((int) out[(std::size_t) channel]
                           - (int) std::lround(linearToSrgb(0.5) * 255.0))
                  <= 1);
};

auto tFormatIntegerTargets = test("Formats/aPassWritesEachIntegerFormat") = []
{
    if (!canTest())
        return;

    auto uintLibrary = makeLibrary(mslFlat("uint4", "uint4(7, 100, 200, 250)"),
                                   hlslFlat("uint4", "uint4(7, 100, 200, 250)"),
                                   glslFlat("uvec4", "uvec4(7u, 100u, 200u, 250u)"));
    auto intLibrary = makeLibrary(mslFlat("int4", "int4(-7, 100, -100, 120)"),
                                  hlslFlat("int4", "int4(-7, 100, -100, 120)"),
                                  glslFlat("ivec4", "ivec4(-7, 100, -100, 120)"));

    for (const auto format: integerFormats)
    {
        const auto signedFormat = isSignedIntegerFormat(format);

        // An Additive preset is dropped on an integer attachment rather than
        // failing the pipeline.
        auto pipeline = makePipeline(signedFormat ? intLibrary : uintLibrary,
                                     pixelFormatFor(format),
                                     BlendMode::Additive);
        check(pipeline.isValid());

        const auto out = drawInto(pipeline, format, targetSize, targetSize, nullptr);
        const auto stride = bytesPerPixel(format);

        check(out.size() == (std::size_t) (texelCount * stride));

        if (out.empty())
            continue;

        for (auto index = 0; index < texelCount; ++index)
            for (auto channel = 0; channel < formatChannels(format); ++channel)
                check(integerOf(format, out.data() + index * stride, channel)
                      == (signedFormat ? flatInts : flatUints)[channel]);
    }
};

namespace
{
constexpr auto mslKernelFloats = R"(
#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d<float, access::write> target [[texture(0)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x < target.get_width() && gid.y < target.get_height())
        target.write(float4(0.25, 0.5, 0.75, 1.0), gid);
}
)";

constexpr auto hlslKernelFloats = R"(
RWTexture2D<float4> target : register(u8);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);

    if (gid.x < width && gid.y < height)
        target[gid.xy] = float4(0.25, 0.5, 0.75, 1.0);
}
)";

std::string mslKernelIntegers(const char* type, const char* value)
{
    return std::string {R"(
#include <metal_stdlib>
using namespace metal;

kernel void computeMain(texture2d<)"}
           + type + R"(, access::write> target [[texture(0)]],
                        uint2 gid [[thread_position_in_grid]])
{
    if (gid.x < target.get_width() && gid.y < target.get_height())
        target.write()"
           + value + R"(, gid);
}
)";
}

std::string hlslKernelIntegers(const char* type, const char* value)
{
    return std::string {"RWTexture2D<"} + type + R"(4> target : register(u8);

[numthreads(8, 8, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    uint width, height;
    target.GetDimensions(width, height);

    if (gid.x < width && gid.y < height)
        target[gid.xy] = )"
           + value + R"(;
}
)";
}

std::string glslKernel(const char* qualifier, const char* prefix, const char* value)
{
    return std::string {R"(#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 8, )"}
           + qualifier + ") uniform writeonly " + prefix + R"(image2D target;

void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(target);

    if (gid.x < size.x && gid.y < size.y)
        imageStore(target, gid, )"
           + value + R"();
}
)";
}

struct KernelFormat
{
    TextureFormat format;
    const char* qualifier;
};

constexpr KernelFormat kernelFloatFormats[] = {
    {TextureFormat::RG16Unorm, "rg16"},
    {TextureFormat::RGBA16Unorm, "rgba16"},
    {TextureFormat::RGB10A2Unorm, "rgb10_a2"},
    {TextureFormat::RG11B10Float, "r11f_g11f_b10f"},
};

constexpr KernelFormat kernelIntegerFormats[] = {
    {TextureFormat::R8Uint, "r8ui"},
    {TextureFormat::RG8Uint, "rg8ui"},
    {TextureFormat::RGBA8Uint, "rgba8ui"},
    {TextureFormat::R16Uint, "r16ui"},
    {TextureFormat::RG16Uint, "rg16ui"},
    {TextureFormat::RGBA16Uint, "rgba16ui"},
    {TextureFormat::RG32Uint, "rg32ui"},
    {TextureFormat::RGBA32Uint, "rgba32ui"},
    {TextureFormat::R8Sint, "r8i"},
    {TextureFormat::RG8Sint, "rg8i"},
    {TextureFormat::RGBA8Sint, "rgba8i"},
    {TextureFormat::R16Sint, "r16i"},
    {TextureFormat::RG16Sint, "rg16i"},
    {TextureFormat::RGBA16Sint, "rgba16i"},
    {TextureFormat::R32Sint, "r32i"},
    {TextureFormat::RG32Sint, "rg32i"},
    {TextureFormat::RGBA32Sint, "rgba32i"},
};

ComputePipeline makeKernel(
    const std::string& msl,
    const std::string& hlsl,
    const std::string& glsl,
    const std::source_location& location = std::source_location::current())
{
    auto source = nativeComputeShaderSource(msl, hlsl, glsl, location)
                      .withCompute("computeMain");
    auto library = Device::shared().makeShaderLibrary(source);
    return Device::shared().makeComputePipeline(library);
}

Vector<std::uint8_t> writeWithKernel(const ComputePipeline& pipeline,
                                     TextureFormat format)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = format;
    descriptor.computeWrite = true;

    auto texture = Device::shared().makeTexture(descriptor);
    auto bytes = Vector<std::uint8_t> {};

    if (!texture.isValid() || !pipeline.isValid())
        return bytes;

    runFrame(
        [&](Frame& frame)
        {
            auto pass = frame.beginCompute();
            pass.setPipeline(pipeline);
            pass.setOutputTexture(texture, 0);
            pass.dispatch(targetSize, targetSize);
        });

    bytes.resize((std::size_t) (texelCount * bytesPerPixel(format)));
    texture.read(bytes.data());
    return bytes;
}
} // namespace

auto tFormatKernelAnswers =
    test("Formats/theDeviceSaysWhichNewFormatsAKernelWrites") = []
{
    if (!canTest())
        return;

    auto& device = Device::shared();

    for (auto value = 0; value <= (int) lastFormat; ++value)
    {
        const auto format = (TextureFormat) value;

        // No API stores into an sRGB, a block or a shared-exponent format from
        // a kernel.
        if (isSrgbFormat(format) || !isRenderableFormat(format))
        {
            check(!device.supportsComputeWrite(format));
            check(!device.supportsComputeReadWrite(format));
        }

        if (device.supportsComputeReadWrite(format))
            check(device.supportsComputeWrite(format));

        // Nothing to average in an integer or a block, and no generator that
        // writes RGB9E5Float.
        if (isIntegerFormat(format) || !isRenderableFormat(format))
            check(!device.supportsMipGeneration(format));
    }

    if constexpr (Platform::isApple())
    {
        for (const auto& written: kernelFloatFormats)
            check(device.supportsComputeWrite(written.format));

        for (const auto& written: kernelIntegerFormats)
            check(device.supportsComputeWrite(written.format));

        check(device.supportsComputeReadWrite(TextureFormat::R32Sint));
        check(!device.supportsComputeReadWrite(TextureFormat::RG16Unorm));

        check(device.supportsMipGeneration(TextureFormat::RGBA8UnormSrgb));
        check(device.supportsMipGeneration(TextureFormat::RGB10A2Unorm));
        check(device.supportsMipGeneration(TextureFormat::RG11B10Float));
    }
};

auto tFormatKernelWrites = test("Formats/aKernelWritesTheNewFormats") = []
{
    if (!canTest())
        return;

    for (const auto& written: kernelFloatFormats)
    {
        if (!Device::shared().supportsComputeWrite(written.format))
            continue;

        auto pipeline = makeKernel(
            mslKernelFloats,
            hlslKernelFloats,
            glslKernel(written.qualifier, "", "vec4(0.25, 0.5, 0.75, 1.0)"));

        const auto out = writeWithKernel(pipeline, written.format);
        const auto stride = bytesPerPixel(written.format);

        check(out.size() == (std::size_t) (texelCount * stride));

        if (out.empty())
            continue;

        for (auto index = 0; index < texelCount; ++index)
            for (auto channel = 0; channel < formatChannels(written.format);
                 ++channel)
                check(near(
                    channelOf(written.format, out.data() + index * stride, channel),
                    flatColor[channel],
                    stepOf(written.format, channel) * 0.75));
    }

    for (const auto& written: kernelIntegerFormats)
    {
        if (!Device::shared().supportsComputeWrite(written.format))
            continue;

        const auto signedFormat = isSignedIntegerFormat(written.format);

        auto pipeline =
            signedFormat
                ? makeKernel(mslKernelIntegers("int", "int4(-7, 100, -100, 120)"),
                             hlslKernelIntegers("int", "int4(-7, 100, -100, 120)"),
                             glslKernel(written.qualifier,
                                        "i",
                                        "ivec4(-7, 100, -100, 120)"))
                : makeKernel(mslKernelIntegers("uint", "uint4(7, 100, 200, 250)"),
                             hlslKernelIntegers("uint", "uint4(7, 100, 200, 250)"),
                             glslKernel(written.qualifier,
                                        "u",
                                        "uvec4(7u, 100u, 200u, 250u)"));

        const auto out = writeWithKernel(pipeline, written.format);
        const auto stride = bytesPerPixel(written.format);

        check(out.size() == (std::size_t) (texelCount * stride));

        if (out.empty())
            continue;

        for (auto index = 0; index < texelCount; ++index)
            for (auto channel = 0; channel < formatChannels(written.format);
                 ++channel)
                check(integerOf(written.format, out.data() + index * stride, channel)
                      == (signedFormat ? flatInts : flatUints)[channel]);
    }
};

namespace
{
// A 4x4 texture of one block, every texel of it the same unless the case says
// otherwise, sampled Nearest into an RGBA32Float target.
Vector<std::uint8_t> sampleBlock(const RenderPipeline& pipeline,
                                 TextureFormat format,
                                 const std::uint8_t* block)
{
    auto source = makeTexture(format, 4, 4, block);
    check(source.isValid());

    if (!source.isValid())
        return {};

    return drawInto(pipeline, TextureFormat::RGBA32Float, 4, 4, &source);
}

// BC4's eight bytes: two endpoints, then sixteen three-bit indices from bit 16,
// texel 0 lowest. Index 0 is the first endpoint and 1 the second.
void writeBC4(std::uint8_t* block,
              std::uint8_t first,
              std::uint8_t second,
              std::uint32_t firstRowIndex,
              std::uint32_t otherIndex)
{
    block[0] = first;
    block[1] = second;

    auto indices = std::uint64_t {0};

    for (auto texel = 0; texel < 16; ++texel)
        indices |= (std::uint64_t) (texel < 4 ? firstRowIndex : otherIndex)
                   << (3 * texel);

    for (auto byte = 0; byte < 6; ++byte)
        block[2 + byte] = (std::uint8_t) ((indices >> (8 * byte)) & 0xffu);
}

// Sets `count` bits of a 128-bit little-endian block from bit `at`.
void putBits(std::uint8_t* block, int at, int count, std::uint32_t value)
{
    for (auto bit = 0; bit < count; ++bit)
        if ((value >> bit) & 1u)
            block[(at + bit) / 8] |= (std::uint8_t) (1u << ((at + bit) % 8));
}

// BC6H mode 11: five mode bits 00011, then two RGB endpoints of ten bits each
// (rw, gw, bw, rx, gx, bx), then the indices. Every endpoint `value` and every
// index 0 makes a block of one colour, unquantised and finished by the format's
// own arithmetic.
void writeBC6H(std::uint8_t* block, std::uint32_t value)
{
    std::memset(block, 0, 16);
    putBits(block, 0, 5, 0x03u);

    for (auto endpoint = 0; endpoint < 6; ++endpoint)
        putBits(block, 5 + 10 * endpoint, 10, value);
}

double bc6hDecode(std::uint32_t value)
{
    const auto unquantised =
        value == 0 ? 0u
                   : (value == 0x3ffu ? 0xffffu : ((value << 16) + 0x8000u) >> 10);

    return halfToFloat((std::uint16_t) ((unquantised * 31u) >> 6));
}

// BC1 with both endpoints `color` and every index 0: one opaque colour.
void writeBC1(std::uint8_t* block, std::uint16_t color)
{
    block[0] = block[2] = (std::uint8_t) (color & 0xffu);
    block[1] = block[3] = (std::uint8_t) (color >> 8);
    block[4] = block[5] = block[6] = block[7] = 0;
}

// BC7 mode 6 (bit 6 set): seven-bit RGBA endpoints with a shared low bit per
// endpoint, then the indices. Every endpoint 0x40 with a p-bit of 0 is 128.
void writeBC7(std::uint8_t* block)
{
    std::memset(block, 0, 16);
    putBits(block, 0, 7, 0x40u);

    for (auto field = 0; field < 8; ++field)
        putBits(block, 7 + 7 * field, 7, 0x40u);
}
} // namespace

auto tFormatBlocks = test("Formats/theNewBlockFormatsDecode") = []
{
    if (!canTest() || !Device::shared().supportsBlockCompression())
        return;

    auto library = makeLibrary(mslSample, hlslSample(0), glslSample);
    auto pipeline = makePipeline(library, PixelFormat::RGBA32Float);

    // BC4 unsigned: 200 on the first row, 100 below it.
    {
        std::uint8_t block[8] = {};
        writeBC4(block, 200, 100, 0, 1);

        const auto out = sampleBlock(pipeline, TextureFormat::BC4RUnorm, block);
        check(out.size() == 256u);

        if (out.size() == 256u)
            for (auto texel = 0; texel < 16; ++texel)
            {
                check(near(floatAt(out, texel, 0),
                           texel < 4 ? 200.0 / 255.0 : 100.0 / 255.0,
                           1e-4));
                check(near(floatAt(out, texel, 1), 0.0, 1e-6));
                check(near(floatAt(out, texel, 3), 1.0, 1e-6));
            }
    }

    // BC4 signed: 64 and -64 of 127.
    {
        std::uint8_t block[8] = {};
        writeBC4(block, 64, (std::uint8_t) -64, 0, 1);

        const auto out = sampleBlock(pipeline, TextureFormat::BC4RSnorm, block);
        check(out.size() == 256u);

        if (out.size() == 256u)
            for (auto texel = 0; texel < 16; ++texel)
                check(near(floatAt(out, texel, 0),
                           texel < 4 ? 64.0 / 127.0 : -64.0 / 127.0,
                           1e-4));
    }

    // BC5: a BC4 block of red, then one of green.
    {
        std::uint8_t block[16] = {};
        writeBC4(block, 200, 100, 0, 1);
        writeBC4(block + 8, 50, 250, 1, 0);

        const auto out = sampleBlock(pipeline, TextureFormat::BC5RGUnorm, block);
        check(out.size() == 256u);

        if (out.size() == 256u)
            for (auto texel = 0; texel < 16; ++texel)
            {
                check(near(floatAt(out, texel, 0),
                           texel < 4 ? 200.0 / 255.0 : 100.0 / 255.0,
                           1e-4));
                check(near(floatAt(out, texel, 1),
                           texel < 4 ? 250.0 / 255.0 : 50.0 / 255.0,
                           1e-4));
                check(near(floatAt(out, texel, 2), 0.0, 1e-6));
            }
    }

    {
        std::uint8_t block[16] = {};
        writeBC4(block, 100, (std::uint8_t) -100, 1, 0);
        writeBC4(block + 8, (std::uint8_t) -27, 127, 0, 1);

        const auto out = sampleBlock(pipeline, TextureFormat::BC5RGSnorm, block);
        check(out.size() == 256u);

        if (out.size() == 256u)
            for (auto texel = 0; texel < 16; ++texel)
            {
                check(near(floatAt(out, texel, 0),
                           texel < 4 ? -100.0 / 127.0 : 100.0 / 127.0,
                           1e-4));
                check(near(
                    floatAt(out, texel, 1), texel < 4 ? -27.0 / 127.0 : 1.0, 1e-4));
            }
    }

    // BC6H: an unsigned half per channel, from a value in the middle of the
    // range and from the largest.
    for (const auto value: {512u, 0x3ffu, 77u})
    {
        std::uint8_t block[16] = {};
        writeBC6H(block, value);

        const auto out = sampleBlock(pipeline, TextureFormat::BC6HRGBUfloat, block);
        check(out.size() == 256u);

        if (out.size() == 256u)
            for (auto texel = 0; texel < 16; ++texel)
            {
                for (auto channel = 0; channel < 3; ++channel)
                    check(near(floatAt(out, texel, channel),
                               bc6hDecode(value),
                               bc6hDecode(value) * 1e-3));

                check(near(floatAt(out, texel, 3), 1.0, 1e-6));
            }
    }

    // The sRGB twins: the same blocks, each channel the decode of what the
    // unsigned format reads. Compared with the device's own unsigned read
    // rather than with a 5:6:5 expansion written here, so the decoders'
    // disagreement over that expansion is not what is measured.
    struct Twin
    {
        TextureFormat linear;
        std::uint8_t block[16];
    };

    auto twins = Vector<Twin> {};

    auto bc1 = Twin {TextureFormat::BC1RGBA, {}};
    writeBC1(bc1.block, 0x8410);
    twins.add(bc1);

    auto bc2 = Twin {TextureFormat::BC2RGBA, {}};
    std::memset(bc2.block, 0xff, 8);
    writeBC1(bc2.block + 8, 0x4208);
    twins.add(bc2);

    auto bc3 = Twin {TextureFormat::BC3RGBA, {}};
    bc3.block[0] = bc3.block[1] = 255;
    writeBC1(bc3.block + 8, 0xc618);
    twins.add(bc3);

    auto bc7 = Twin {TextureFormat::BC7RGBA, {}};
    writeBC7(bc7.block);
    twins.add(bc7);

    for (const auto& twin: twins)
    {
        const auto linear = sampleBlock(pipeline, twin.linear, twin.block);
        const auto encoded =
            sampleBlock(pipeline, srgbFormatFor(twin.linear), twin.block);

        check(linear.size() == 256u && encoded.size() == 256u);

        if (linear.size() != 256u || encoded.size() != 256u)
            continue;

        for (auto channel = 0; channel < 3; ++channel)
        {
            const auto value = floatAt(linear, 5, channel);

            check(value > 0.05f);
            check(near(floatAt(encoded, 5, channel), srgbToLinear(value), 2.5e-3));
        }

        check(near(floatAt(encoded, 5, 3), floatAt(linear, 5, 3), 1e-3));
    }

    // BC7's 0x40 endpoints with a p-bit of 0 are 128 exactly.
    {
        const auto out =
            sampleBlock(pipeline, TextureFormat::BC7RGBA, twins[3].block);

        if (out.size() == 256u)
            for (auto channel = 0; channel < 4; ++channel)
                check(near(floatAt(out, 0, channel), 128.0 / 255.0, 1e-4));
    }
};

auto tFormatCpuChains = test("Formats/theCpuChainAveragesTheNewFormats") = []
{
    // sRGB: black and white average to half the light, stored as 188, while the
    // alpha averages as the number it is.
    {
        constexpr std::uint8_t pixels[] = {0, 0, 0, 0, 255, 255, 255, 255};
        const auto chain =
            buildMipChain(pixels, 2, 1, TextureFormat::RGBA8UnormSrgb);

        check(chain.levelCount() == 2);

        if (chain.levelCount() == 2)
        {
            const auto* level = static_cast<const std::uint8_t*>(chain.level(1));

            for (auto channel = 0; channel < 3; ++channel)
                check(level[channel]
                      == (std::uint8_t) std::lround(linearToSrgb(0.5) * 255.0));

            check(level[3] == 128);
        }

        // RGBA8Unorm holding the same bytes averages the code values.
        const auto plain = buildMipChain(pixels, 2, 1, TextureFormat::RGBA8Unorm);
        check(static_cast<const std::uint8_t*>(plain.level(1))[0] == 128);
    }

    // RGB10A2: each field on its own.
    {
        const std::uint32_t pixels[] = {packRGB10A2(100, 1023, 0, 3),
                                        packRGB10A2(200, 1, 1000, 1)};
        const auto chain = buildMipChain(pixels, 2, 1, TextureFormat::RGB10A2Unorm);

        check(chain.levelCount() == 2);

        if (chain.levelCount() == 2)
        {
            auto word = std::uint32_t {};
            std::memcpy(&word, chain.level(1), 4);
            check(word == packRGB10A2(150, 512, 500, 2));
        }
    }

    // RG11B10Float: through float, back to the nearest of each width.
    {
        const std::uint32_t pixels[] = {packRG11B10(1.f, 0.25f, 8.f),
                                        packRG11B10(3.f, 0.75f, 0.f)};
        const auto chain = buildMipChain(pixels, 2, 1, TextureFormat::RG11B10Float);

        check(chain.levelCount() == 2);

        if (chain.levelCount() == 2)
        {
            auto word = std::uint32_t {};
            std::memcpy(&word, chain.level(1), 4);
            check(word == packRG11B10(2.f, 0.5f, 4.f));
        }
    }

    // RG16Unorm and RGBA16Unorm: two and four 16-bit channels.
    {
        const std::uint16_t pixels[] = {0, 65535, 1000, 3000};
        const auto chain = buildMipChain(pixels, 2, 1, TextureFormat::RG16Unorm);

        check(chain.levelCount() == 2);

        if (chain.levelCount() == 2)
        {
            const auto* level = static_cast<const std::uint16_t*>(chain.level(1));
            check(level[0] == 500 && level[1] == 34268);
        }

        const std::uint16_t wide[] = {0, 2, 4, 6, 10, 12, 14, 16};
        const auto four = buildMipChain(wide, 2, 1, TextureFormat::RGBA16Unorm);

        if (four.levelCount() == 2)
        {
            const auto* level = static_cast<const std::uint16_t*>(four.level(1));
            check(level[0] == 5 && level[1] == 7 && level[2] == 9 && level[3] == 11);
        }
    }

    check(!buildMipChain(flatUints, 1, 1, TextureFormat::RGBA16Uint).isValid());
};

auto tFormatGpuSrgbChain =
    test("Formats/theGpuChainOfAnSrgbTextureAveragesLight") = []
{
    if (!canTest())
        return;

    // Through `mipmapped` (the CPU chain) and through generateMipmaps (the
    // GPU's), the 1x1 level of a black-and-white checker is half the light.
    constexpr std::uint8_t black[] = {0, 0, 0, 255};
    constexpr std::uint8_t white[] = {255, 255, 255, 255};

    auto pixels = Vector<std::uint8_t> {};

    for (auto texel = 0; texel < 4; ++texel)
        for (auto byte = 0; byte < 4; ++byte)
            pixels.add(((texel + texel / 2) % 2 == 0 ? black : white)[byte]);

    const auto expected = (int) std::lround(linearToSrgb(0.5) * 255.0);

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;
    descriptor.format = TextureFormat::RGBA8UnormSrgb;
    descriptor.mipmapped = true;

    auto cpu = Device::shared().makeTexture(descriptor, pixels.data());
    check(cpu.isValid() && cpu.mipLevels() == 2);

    std::uint8_t texel[4] = {};
    cpu.readSubresource(TextureRegion::wholeLevel(1), texel);

    for (auto channel = 0; channel < 3; ++channel)
        check(std::abs((int) texel[channel] - expected) <= 1);

    if (!Device::shared().supportsMipGeneration(TextureFormat::RGBA8UnormSrgb))
        return;

    descriptor.mipmapped = false;
    descriptor.mipLevels = 2;
    descriptor.renderTarget = true;

    auto gpu = Device::shared().makeTexture(descriptor);
    check(gpu.isValid());

    gpu.updateSubresource(TextureRegion::wholeLevel(0), pixels.data());
    runFrame([&](Frame& frame) { frame.generateMipmaps(gpu); });

    std::uint8_t generated[4] = {};
    gpu.readSubresource(TextureRegion::wholeLevel(1), generated);

    // The API's own box filter, in light; a little looser than the CPU's.
    for (auto channel = 0; channel < 3; ++channel)
        check(std::abs((int) generated[channel] - expected) <= 3);
};

namespace
{
// RGB9E5Float written down independently of eacp's encoder: nine-bit
// mantissas, red lowest, and the shared exponent in the top five bits, a value
// being mantissa * 2^(exponent - 24).
std::uint32_t
    packRGB9E5(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t e)
{
    return r | (g << 9) | (b << 18) | (e << 27);
}

double rgb9e5Channel(std::uint32_t word, int channel)
{
    const auto mantissa = (word >> (9 * channel)) & 0x1ffu;
    return std::ldexp((double) mantissa, (int) (word >> 27) - 24);
}

// One texel per exponent region: small, around 1, large, the largest finite
// value, and zero.
const std::uint32_t rgb9e5Texels[] = {
    packRGB9E5(256, 64, 32, 18), // 4, 1, 0.5
    packRGB9E5(384, 192, 0, 17), // 3, 1.5, 0
    packRGB9E5(511, 1, 300, 31), // 65408, 128, 38400
    packRGB9E5(1, 2, 3, 0), // 2^-24, 2^-23, 3 * 2^-24
    packRGB9E5(0, 0, 0, 0), // 0
    packRGB9E5(500, 250, 125, 10), // 500, 250, 125 over 16384
    packRGB9E5(17, 33, 65, 24), // 17, 33, 65
    packRGB9E5(256, 256, 256, 16), // 1, 1, 1
};
} // namespace

auto tFormatSharedExponentRoundTrip =
    test("Formats/rgb9e5UploadsReadsBackAndIsNoTarget") = []
{
    if (!canTest())
        return;

    auto bytes = Vector<std::uint8_t> {};
    bytes.resize((std::size_t) (texelCount * 4));

    for (auto texel = 0; texel < texelCount; ++texel)
    {
        const auto word = rgb9e5Texels[texel % std::size(rgb9e5Texels)];
        std::memcpy(bytes.data() + texel * 4, &word, 4);
    }

    auto texture = makeTexture(
        TextureFormat::RGB9E5Float, targetSize, targetSize, bytes.data());

    check(texture.isValid());
    check(texture.format() == TextureFormat::RGB9E5Float);

    if (texture.isValid())
    {
        auto back = Vector<std::uint8_t> {};
        back.resize(bytes.size());
        texture.read(back.data());
        check(std::memcmp(back.data(), bytes.data(), bytes.size()) == 0);

        // A region update moves the words untouched too.
        const auto word = packRGB9E5(7, 8, 9, 20);
        texture.update({1.f, 2.f, 1.f, 1.f}, &word);
        texture.read(back.data());

        auto got = std::uint32_t {};
        std::memcpy(&got, back.data() + (2 * targetSize + 1) * 4, 4);
        check(got == word);
    }

    // Sampled only: no render target, no kernel output, on any device.
    check(!makeTexture(
               TextureFormat::RGB9E5Float, targetSize, targetSize, nullptr, true)
               .isValid());

    auto written = TextureDescriptor {};
    written.width = targetSize;
    written.height = targetSize;
    written.format = TextureFormat::RGB9E5Float;
    written.computeWrite = true;
    check(!Device::shared().makeTexture(written).isValid());

    check(!Device::shared().supportsComputeWrite(TextureFormat::RGB9E5Float));
    check(!Device::shared().supportsMipGeneration(TextureFormat::RGB9E5Float));
};

auto tFormatSharedExponentSample = test("Formats/rgb9e5SamplesAsItsDecode") = []
{
    if (!canTest())
        return;

    auto library = makeLibrary(mslSample, hlslSample(0), glslSample);
    auto pipeline = makePipeline(library, PixelFormat::RGBA32Float);

    auto pixels = Vector<std::uint8_t> {};
    pixels.resize((std::size_t) (texelCount * 4));

    for (auto texel = 0; texel < texelCount; ++texel)
    {
        const auto word = rgb9e5Texels[texel % std::size(rgb9e5Texels)];
        std::memcpy(pixels.data() + texel * 4, &word, 4);
    }

    auto source = makeTexture(
        TextureFormat::RGB9E5Float, targetSize, targetSize, pixels.data());
    check(source.isValid());

    const auto out = drawInto(
        pipeline, TextureFormat::RGBA32Float, targetSize, targetSize, &source);
    check(out.size() == (std::size_t) (texelCount * 16));

    if (out.empty())
        return;

    for (auto texel = 0; texel < texelCount; ++texel)
    {
        const auto word = rgb9e5Texels[texel % std::size(rgb9e5Texels)];

        // Every value the format holds is a float exactly, so the sample is
        // the decode to the bit.
        for (auto channel = 0; channel < 3; ++channel)
        {
            const auto expected = rgb9e5Channel(word, channel);
            check(near(floatAt(out, texel, channel), expected, expected * 1e-6));
        }

        check(floatAt(out, texel, 3) == 1.f);
    }
};

auto tFormatSharedExponentChain =
    test("Formats/rgb9e5ChainIsAveragedThroughFloat") = []
{
    // Two texels whose averages are exact: (4, 1, 0.5) and (3, 1.5, 0) make
    // (3.5, 1.25, 0.25), stored at the brightest channel's exponent (2^1 <=
    // 3.5 < 2^2, so 2^-7 a step: 448, 160, 32 at exponent 17).
    {
        const std::uint32_t pixels[] = {rgb9e5Texels[0], rgb9e5Texels[1]};
        const auto chain = buildMipChain(pixels, 2, 1, TextureFormat::RGB9E5Float);

        check(chain.levelCount() == 2);

        if (chain.levelCount() == 2)
        {
            auto word = std::uint32_t {};
            std::memcpy(&word, chain.level(1), 4);
            check(word == packRGB9E5(448, 160, 32, 17));
            check(rgb9e5Channel(word, 0) == 3.5 && rgb9e5Channel(word, 1) == 1.25
                  && rgb9e5Channel(word, 2) == 0.25);
        }
    }

    // A mean that rounds its brightest mantissa up past nine bits moves to the
    // next exponent: 511 * 2^-8 and 256 * 2^-7 (2.0) average to 511.5 * 2^-8,
    // which rounds to 512 at exponent 16 and so is stored as 256 at 17.
    {
        const std::uint32_t pixels[] = {packRGB9E5(511, 0, 0, 16),
                                        packRGB9E5(256, 0, 0, 17)};
        const auto chain = buildMipChain(pixels, 2, 1, TextureFormat::RGB9E5Float);

        if (chain.levelCount() == 2)
        {
            auto word = std::uint32_t {};
            std::memcpy(&word, chain.level(1), 4);
            check(word == packRGB9E5(256, 0, 0, 17));
        }
    }

    if (!canTest())
        return;

    // Through `mipmapped`: a 2x2 of the first two texels twice over keeps the
    // CPU's 1x1 level, and the GPU samples that level as its decode.
    const std::uint32_t pixels[] = {
        rgb9e5Texels[0], rgb9e5Texels[1], rgb9e5Texels[1], rgb9e5Texels[0]};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;
    descriptor.format = TextureFormat::RGB9E5Float;
    descriptor.mipmapped = true;

    auto texture = Device::shared().makeTexture(descriptor, pixels);
    check(texture.isValid() && texture.mipLevels() == 2);

    if (texture.mipLevels() != 2)
        return;

    auto word = std::uint32_t {};
    texture.readSubresource(TextureRegion::wholeLevel(1), &word);
    check(word == packRGB9E5(448, 160, 32, 17));
};
