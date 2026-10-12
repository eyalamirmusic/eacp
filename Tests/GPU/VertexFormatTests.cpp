#include "Common.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <string>

// Packed vertex formats, checked by rendering the same picture twice - once
// with the attribute unpacked to floats, once packed - and comparing the two
// images to each other.
//
// Against each other rather than against expected colours, deliberately. The
// snapshot path returns premultiplied pixels, so absolute values are a trap the
// README already warns about; and the question here is not what a red quad
// looks like, it is whether the packed path draws the same thing the float path
// does. A wrong format mapping is not a build error on either backend - the
// pipeline is built, the draw happens, and the geometry or the colour is simply
// wrong - so a comparison is the only thing that catches it.
//
// The two mapping tables are hand-written per backend and cannot be checked
// against each other from one machine, which is what makes this the file to run
// on both.
//
// Self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewSize = 32.f;

// Wide enough to absorb the difference between interpolating a float attribute
// and interpolating one the hardware just widened, narrow enough that a wrong
// format - which misreads the bytes entirely - cannot slip through.
constexpr auto tolerance = 3.0f / 255.0f;

bool matches(const Graphics::Image& a, const Graphics::Image& b)
{
    if (!a.isValid() || !b.isValid() || a.width() != b.width()
        || a.height() != b.height())
        return false;

    for (auto y = 0; y < a.height(); ++y)
    {
        for (auto x = 0; x < a.width(); ++x)
        {
            const auto first = a.at(x, y);
            const auto second = b.at(x, y);

            if (std::abs(first.r - second.r) > tolerance
                || std::abs(first.g - second.g) > tolerance
                || std::abs(first.b - second.b) > tolerance)
                return false;
        }
    }

    return true;
}

// Not every pair of images that matches proves anything: two black frames match
// too. Every case below checks this first, so a comparison that passed because
// nothing was drawn fails instead.
bool hasColour(const Graphics::Image& image)
{
    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            if (image.at(x, y).r > 0.1f || image.at(x, y).g > 0.1f
                || image.at(x, y).b > 0.1f)
                return true;

    return false;
}

// A quad covering the whole target, so every pixel of the comparison carries a
// different interpolated attribute value rather than one flat colour.
constexpr float quadCorners[6][2] = {
    {-1.f, -1.f},
    {1.f, -1.f},
    {-1.f, 1.f},
    {1.f, -1.f},
    {1.f, 1.f},
    {-1.f, 1.f},
};

constexpr float cornerColours[6][4] = {
    {1.f, 0.f, 0.f, 1.f},
    {0.f, 1.f, 0.f, 1.f},
    {0.f, 0.f, 1.f, 1.f},
    {0.f, 1.f, 0.f, 1.f},
    {1.f, 1.f, 0.f, 1.f},
    {0.f, 0.f, 1.f, 1.f},
};

constexpr float cornerUVs[6][2] = {
    {0.f, 0.f},
    {1.f, 0.f},
    {0.f, 1.f},
    {1.f, 0.f},
    {1.f, 1.f},
    {0.f, 1.f},
};

// One program per storage type, with identical define() bodies. That the bodies
// are identical is the point: only the CPU struct differs, so anything that
// shows up in the pixels came from the wire format.
template <typename Vertex>
struct ColourProgram final : ShaderProgram
{
    ColourProgram() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto colour = vertexInput(&Vertex::colour);

        setPosition(float4(position.x(), position.y(), 0.0f, 1.0f));
        setFragment(varying(colour));
    }

    // No uniforms at all - the attribute is the whole shader. reflectMembers is
    // pure virtual, so it still needs the macro, with nothing in it.
    EACP_SHADER()
};

template <typename Vertex>
struct TexturedProgram final : ShaderProgram
{
    TexturedProgram() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto uv = vertexInput(&Vertex::uv);

        setPosition(float4(position.x(), position.y(), 0.0f, 1.0f));
        setFragment(sample(image, varying(uv)));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

template <typename Program, typename Vertex>
struct ProgramView final : GPUView
{
    explicit ProgramView(const Vertex (&vertices)[6])
    {
        setSampleCount(1);
        setBounds({0.f, 0.f, viewSize, viewSize});

        program.setVertices(vertices);
        program.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color::black()});
        pass.draw(program);
    }

    Program program;
};

Texture quadrantTexture()
{
    // Four distinct texels, so where a UV lands is visible in the colour.
    const unsigned char pixels[] = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;
    descriptor.format = TextureFormat::RGBA8Unorm;

    return Device::shared().makeTexture(descriptor, pixels);
}

struct FloatColourVertex
{
    float position[2];
    float colour[4];
};

struct PackedColourVertex
{
    float position[2];
    UNorm8x4 colour;
};

struct ShortColourVertex
{
    float position[2];
    SNorm16x4 colour;
};

struct FloatUVVertex
{
    float position[2];
    float uv[2];
};

struct HalfUVVertex
{
    float position[2];
    Float16x2 uv;
};
} // namespace

// The headline case, and the one this repo's ImGui backend needs: a colour is
// four bytes, not four floats, and the shader must not be able to tell.
auto tUNorm8MatchesFloat4 = test("VertexFormat/uNorm8x4MatchesFloat4") = []
{
    if (!Device::shared().isValid())
        return;

    FloatColourVertex floatVertices[6] {};
    PackedColourVertex packedVertices[6] {};

    for (auto i = 0; i < 6; ++i)
    {
        floatVertices[i] = {{quadCorners[i][0], quadCorners[i][1]},
                            {cornerColours[i][0],
                             cornerColours[i][1],
                             cornerColours[i][2],
                             cornerColours[i][3]}};

        packedVertices[i] = {{quadCorners[i][0], quadCorners[i][1]},
                             UNorm8x4::fromFloats(cornerColours[i][0],
                                                  cornerColours[i][1],
                                                  cornerColours[i][2],
                                                  cornerColours[i][3])};
    }

    auto floatView =
        ProgramView<ColourProgram<FloatColourVertex>, FloatColourVertex> {
            floatVertices};
    auto packedView =
        ProgramView<ColourProgram<PackedColourVertex>, PackedColourVertex> {
            packedVertices};

    if (!floatView.program.pipeline().isValid()
        || !packedView.program.pipeline().isValid())
        return;

    const auto expected = floatView.renderToImage(1.f);
    const auto packed = packedView.renderToImage(1.f);

    check(hasColour(expected));
    check(matches(expected, packed));
};

// The vertex shrinks, which is the entire reason for any of this. Stated as a
// compile-time fact so it cannot quietly regress.
auto tPackedVertexIsSmaller = test("VertexFormat/packingShrinksTheVertex") = []
{
    static_assert(sizeof(PackedColourVertex) == 12);
    static_assert(sizeof(FloatColourVertex) == 24);
    static_assert(sizeof(HalfUVVertex) == 12);
    static_assert(sizeof(FloatUVVertex) == 16);

    check(true);
};

// Signed normalized shorts, the storage a normal or a tangent wants. Drawn with
// positive values only, so the comparison is against the same picture the float
// path draws rather than a differently-scaled one.
auto tSNorm16MatchesFloat4 = test("VertexFormat/sNorm16x4MatchesFloat4") = []
{
    if (!Device::shared().isValid())
        return;

    FloatColourVertex floatVertices[6] {};
    ShortColourVertex shortVertices[6] {};

    for (auto i = 0; i < 6; ++i)
    {
        floatVertices[i] = {{quadCorners[i][0], quadCorners[i][1]},
                            {cornerColours[i][0],
                             cornerColours[i][1],
                             cornerColours[i][2],
                             cornerColours[i][3]}};

        shortVertices[i] = {{quadCorners[i][0], quadCorners[i][1]},
                            SNorm16x4::from(cornerColours[i][0],
                                            cornerColours[i][1],
                                            cornerColours[i][2],
                                            cornerColours[i][3])};
    }

    auto floatView =
        ProgramView<ColourProgram<FloatColourVertex>, FloatColourVertex> {
            floatVertices};
    auto shortView =
        ProgramView<ColourProgram<ShortColourVertex>, ShortColourVertex> {
            shortVertices};

    if (!floatView.program.pipeline().isValid()
        || !shortView.program.pipeline().isValid())
        return;

    const auto expected = floatView.renderToImage(1.f);
    const auto packed = shortView.renderToImage(1.f);

    check(hasColour(expected));
    check(matches(expected, packed));
};

// Half UVs against float UVs, sampling a texture whose four texels are far
// apart in colour - so a UV that lands in the wrong quadrant is a large,
// obvious difference rather than a rounding one.
auto tHalfUVsMatchFloatUVs = test("VertexFormat/half2UVsMatchFloat2") = []
{
    if (!Device::shared().isValid())
        return;

    auto texture = quadrantTexture();

    if (!texture.isValid())
        return;

    FloatUVVertex floatVertices[6] {};
    HalfUVVertex halfVertices[6] {};

    for (auto i = 0; i < 6; ++i)
    {
        floatVertices[i] = {{quadCorners[i][0], quadCorners[i][1]},
                            {cornerUVs[i][0], cornerUVs[i][1]}};

        halfVertices[i] = {{quadCorners[i][0], quadCorners[i][1]},
                           Float16x2::from(cornerUVs[i][0], cornerUVs[i][1])};
    }

    auto floatView =
        ProgramView<TexturedProgram<FloatUVVertex>, FloatUVVertex> {floatVertices};
    auto halfView =
        ProgramView<TexturedProgram<HalfUVVertex>, HalfUVVertex> {halfVertices};

    if (!floatView.program.pipeline().isValid()
        || !halfView.program.pipeline().isValid())
        return;

    floatView.program.image = texture;
    halfView.program.image = texture;

    const auto expected = floatView.renderToImage(1.f);
    const auto packed = halfView.renderToImage(1.f);

    check(hasColour(expected));
    check(matches(expected, packed));
};

// The half conversion itself, away from any GPU. It is the one piece of this
// that is arithmetic rather than a table lookup, so it is worth pinning
// directly rather than only through what a shader made of it.
auto tHalfRoundTrips = test("VertexFormat/halfRoundTripsExactValues") = []
{
    // Exactly representable in half, so the round trip has to be exact.
    const float exact[] = {0.f, 1.f, -1.f, 0.5f, -0.25f, 2.f, 1024.f, 65504.f};

    for (auto value: exact)
        check(halfToFloat(halfFromFloat(value)) == value);

    // Ten bits of mantissa, so anything in range survives to about one part in
    // a thousand - the precision a UV needs and a position does not.
    const float approximate[] = {0.1f, 0.333f, 12.345f, -678.9f};

    for (auto value: approximate)
        check(std::abs(halfToFloat(halfFromFloat(value)) - value)
              <= std::abs(value) * 0.001f);
};

// The edges, where a naive conversion goes wrong quietly: too large should
// saturate rather than wrap to zero, and too small should reach zero through
// the subnormals rather than falling off a cliff.
auto tHalfHandlesTheEdges = test("VertexFormat/halfSaturatesAndUnderflows") = []
{
    check(std::isinf(halfToFloat(halfFromFloat(70000.f))));
    check(halfToFloat(halfFromFloat(70000.f)) > 0.f);
    check(std::isinf(halfToFloat(halfFromFloat(-70000.f))));
    check(halfToFloat(halfFromFloat(-70000.f)) < 0.f);

    // The smallest subnormal, and half of it - which rounds to zero.
    const auto smallestSubnormal = std::ldexp(1.f, -24);
    check(halfToFloat(halfFromFloat(smallestSubnormal)) == smallestSubnormal);
    check(halfToFloat(halfFromFloat(smallestSubnormal / 2.f)) == 0.f);

    // Below the smallest normal but well inside subnormal range, so it must
    // come back as something rather than being flushed to zero.
    check(halfToFloat(halfFromFloat(std::ldexp(1.f, -20))) > 0.f);

    check(std::isnan(halfToFloat(halfFromFloat(std::nanf("")))));
};

// Every format past the EDSL's packed five, each fed to a hand-written shader
// that writes the attribute it read straight out as the colour of an RGBA32F
// target, read back exactly. The integer formats go into uint4 or int4 inputs
// and the normalised ones into float4, which is the rule VertexLayout.h states
// and the one D3D12 and Vulkan hold a pipeline to; components the format lacks
// read as 0, 0, 1.
namespace
{
constexpr auto attributeTargetSize = 4;

enum class InputKind
{
    Float,
    UInt,
    Int
};

std::string attributeMsl(InputKind kind)
{
    const auto* type = kind == InputKind::Float  ? "float4"
                       : kind == InputKind::UInt ? "uint4"
                                                 : "int4";

    return std::string(R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
    float2 position [[attribute(0)]];
    )") + type + R"( value [[attribute(1)]];
};

struct VertexOut
{
    float4 position [[position]];
    float4 colour [[flat]];
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]])
{
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.colour = float4(in.value);
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]])
{
    return in.colour;
}
)";
}

std::string attributeHlsl(InputKind kind)
{
    const auto* type = kind == InputKind::Float  ? "float4"
                       : kind == InputKind::UInt ? "uint4"
                                                 : "int4";

    return std::string(R"(
struct VertexOut
{
    float4 position : SV_Position;
    nointerpolation float4 colour : TEXCOORD0;
};

VertexOut vertexMain(float2 position : TEXCOORD0, )")
           + type + R"( value : TEXCOORD1)
{
    VertexOut o;
    o.position = float4(position, 0.0, 1.0);
    o.colour = float4(value);
    return o;
}

float4 fragmentMain(VertexOut input) : SV_Target { return input.colour; }
)";
}

std::string attributeGlsl(InputKind kind)
{
    const auto* type = kind == InputKind::Float  ? "vec4"
                       : kind == InputKind::UInt ? "uvec4"
                                                 : "ivec4";

    return std::string(R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 1) in )")
           + type + R"( attr1;
layout(location = 0) flat out vec4 colour;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
    colour = vec4(attr1);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) flat in vec4 colour;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = colour;
}
#endif
)";
}

struct AttributeRunner final : GPUView
{
    AttributeRunner() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        draw(frame);
        frame.beginPass({Graphics::Color::black()});
    }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

// A position and then 16 bytes of attribute, of which the format reads its
// own size from the start.
struct AttributeVertex
{
    float position[2];
    std::uint8_t value[16];
};

struct AttributeCase
{
    VertexFormat format;
    InputKind kind;
    std::uint8_t bytes[16];
    float expected[4];
    const char* name;
};

template <typename T, std::size_t N>
AttributeCase attributeCase(VertexFormat format,
                            InputKind kind,
                            const T (&values)[N],
                            std::array<float, 4> expected,
                            const char* name)
{
    static_assert(sizeof(T) * N <= 16);

    auto result = AttributeCase {format, kind, {}, {}, name};
    std::memcpy(result.bytes, values, sizeof(T) * N);

    for (auto i = 0; i < 4; ++i)
        result.expected[i] = expected[(std::size_t) i];

    return result;
}

Vector<AttributeCase> attributeCases()
{
    using F = VertexFormat;
    using K = InputKind;

    const std::uint8_t ubytes[] = {7, 200, 0, 255};
    const std::int8_t bytes[] = {-5, 100, -128, 127};
    const std::uint8_t ubyteNorms[] = {51, 255};
    const std::int8_t byteNorms[] = {-127, 127, 0, 127};
    const std::uint16_t ushorts[] = {60000, 3, 0, 65535};
    const std::uint16_t ushortNorms[] = {65535, 0, 65535, 0};
    const std::int16_t shorts[] = {-30000, 30000, -1, 1};
    const std::int16_t shortNorms[] = {-32767};
    const std::uint16_t half[] = {0x3800}; // 0.5
    const std::uint32_t packed[] = {1023u | (0u << 10) | (341u << 20) | (1u << 30)};
    const std::uint32_t uints[] = {123456u, 7u, 0u, 4000000000u};
    const std::int32_t ints[] = {-123456, 7, 0, 2000000000};

    auto cases = Vector<AttributeCase> {};

    cases.add(attributeCase(F::UByte, K::UInt, ubytes, {7, 0, 0, 1}, "UByte"));
    cases.add(attributeCase(F::UByte2, K::UInt, ubytes, {7, 200, 0, 1}, "UByte2"));
    cases.add(
        attributeCase(F::UByte4, K::UInt, ubytes, {7, 200, 0, 255}, "UByte4"));
    cases.add(attributeCase(
        F::UByteNorm, K::Float, ubyteNorms, {0.2f, 0, 0, 1}, "UByteNorm"));
    cases.add(attributeCase(
        F::UByte2Norm, K::Float, ubyteNorms, {0.2f, 1, 0, 1}, "UByte2Norm"));
    cases.add(attributeCase(F::Byte, K::Int, bytes, {-5, 0, 0, 1}, "Byte"));
    cases.add(attributeCase(F::Byte2, K::Int, bytes, {-5, 100, 0, 1}, "Byte2"));
    cases.add(
        attributeCase(F::Byte4, K::Int, bytes, {-5, 100, -128, 127}, "Byte4"));
    cases.add(
        attributeCase(F::ByteNorm, K::Float, byteNorms, {-1, 0, 0, 1}, "ByteNorm"));
    cases.add(attributeCase(
        F::Byte2Norm, K::Float, byteNorms, {-1, 1, 0, 1}, "Byte2Norm"));
    cases.add(attributeCase(
        F::Byte4Norm, K::Float, byteNorms, {-1, 1, 0, 1}, "Byte4Norm"));
    cases.add(attributeCase(F::UShort, K::UInt, ushorts, {60000, 0, 0, 1}, "UShort"));
    cases.add(
        attributeCase(F::UShort2, K::UInt, ushorts, {60000, 3, 0, 1}, "UShort2"));
    cases.add(attributeCase(
        F::UShort4, K::UInt, ushorts, {60000, 3, 0, 65535}, "UShort4"));
    cases.add(attributeCase(
        F::UShortNorm, K::Float, ushortNorms, {1, 0, 0, 1}, "UShortNorm"));
    cases.add(attributeCase(
        F::UShort2Norm, K::Float, ushortNorms, {1, 0, 0, 1}, "UShort2Norm"));
    cases.add(attributeCase(
        F::UShort4Norm, K::Float, ushortNorms, {1, 0, 1, 0}, "UShort4Norm"));
    cases.add(attributeCase(F::Short, K::Int, shorts, {-30000, 0, 0, 1}, "Short"));
    cases.add(
        attributeCase(F::Short2, K::Int, shorts, {-30000, 30000, 0, 1}, "Short2"));
    cases.add(attributeCase(
        F::Short4, K::Int, shorts, {-30000, 30000, -1, 1}, "Short4"));
    cases.add(attributeCase(
        F::ShortNorm, K::Float, shortNorms, {-1, 0, 0, 1}, "ShortNorm"));
    cases.add(attributeCase(F::Half, K::Float, half, {0.5f, 0, 0, 1}, "Half"));
    cases.add(attributeCase(F::UInt1010102Norm,
                            K::Float,
                            packed,
                            {1, 0, 341.f / 1023.f, 1.f / 3.f},
                            "UInt1010102Norm"));
    cases.add(attributeCase(F::UInt, K::UInt, uints, {123456, 0, 0, 1}, "UInt"));
    cases.add(attributeCase(F::UInt2, K::UInt, uints, {123456, 7, 0, 1}, "UInt2"));
    cases.add(attributeCase(F::UInt3, K::UInt, uints, {123456, 7, 0, 1}, "UInt3"));
    cases.add(attributeCase(
        F::UInt4, K::UInt, uints, {123456, 7, 0, 4000000000.f}, "UInt4"));
    cases.add(attributeCase(F::Int, K::Int, ints, {-123456, 0, 0, 1}, "Int"));
    cases.add(attributeCase(F::Int2, K::Int, ints, {-123456, 7, 0, 1}, "Int2"));
    cases.add(attributeCase(F::Int3, K::Int, ints, {-123456, 7, 0, 1}, "Int3"));
    cases.add(attributeCase(
        F::Int4, K::Int, ints, {-123456, 7, 0, 2000000000.f}, "Int4"));

    return cases;
}

// What the attribute read as, at one texel in the middle of the target, or
// NaNs where nothing could be drawn.
std::array<float, 4> drawAttribute(const ShaderLibrary& library,
                                   const AttributeCase& entry)
{
    auto none = std::numeric_limits<float>::quiet_NaN();
    auto result = std::array<float, 4> {none, none, none, none};

    auto vertices = std::array<AttributeVertex, 6> {};

    for (auto i = 0; i < 6; ++i)
    {
        vertices[(std::size_t) i].position[0] = quadCorners[i][0];
        vertices[(std::size_t) i].position[1] = quadCorners[i][1];
        std::memcpy(vertices[(std::size_t) i].value, entry.bytes, 16);
    }

    auto quad = Device::shared().makeBuffer(vertices.data(), sizeof(vertices));

    auto descriptor = TextureDescriptor {};
    descriptor.width = attributeTargetSize;
    descriptor.height = attributeTargetSize;
    descriptor.format = TextureFormat::RGBA32Float;
    descriptor.renderTarget = true;

    auto texture = Device::shared().makeTexture(descriptor);

    auto pipelineDescriptor = RenderPipelineDescriptor {};
    pipelineDescriptor.library = &library;
    pipelineDescriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA32Float);
    pipelineDescriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
    pipelineDescriptor.vertexLayout.attribute(
        entry.format, (int) offsetof(AttributeVertex, value));
    pipelineDescriptor.vertexLayout.stride = sizeof(AttributeVertex);

    auto pipeline = Device::shared().makeRenderPipeline(pipelineDescriptor);

    if (!texture.isValid() || !pipeline.isValid())
        return result;

    auto view = AttributeRunner {};
    view.draw = [&](Frame& frame)
    {
        auto pass = frame.beginPass(texture, {Graphics::Color::black()});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(quad);
        pass.draw(6);
    };
    view.setBounds({0.f, 0.f, 4.f, 4.f});
    view.renderToImage(1.f);

    float texels[attributeTargetSize * attributeTargetSize * 4] = {};
    texture.read(texels);

    const auto centre = (attributeTargetSize + 1) * (attributeTargetSize / 2) * 4;

    for (auto i = 0; i < 4; ++i)
        result[(std::size_t) i] = texels[centre + i];

    return result;
}

bool readAsExpected(const std::array<float, 4>& read, const AttributeCase& entry)
{
    for (auto i = 0; i < 4; ++i)
    {
        const auto want = entry.expected[i];
        const auto slack = std::max(1e-3f, std::abs(want) * 1e-6f);

        if (!(std::abs(read[(std::size_t) i] - want) <= slack))
        {
            LOG("VertexFormat ",
                entry.name,
                ": component ",
                i,
                " read ",
                read[(std::size_t) i],
                ", expected ",
                want);
            return false;
        }
    }

    return true;
}
} // namespace

static_assert(bytesPerAttribute(VertexFormat::UInt1010102Norm) == 4);
static_assert(bytesPerAttribute(VertexFormat::UShort4) == 8);
static_assert(bytesPerAttribute(VertexFormat::Int3) == 12);

auto tIntegerFormatsAreTheUnnormalisedOnes =
    test("VertexFormat/integerFormatsAreTheUnnormalisedOnes") = []
{
    check(isIntegerVertexFormat(VertexFormat::UByte4));
    check(isIntegerVertexFormat(VertexFormat::Int3));
    check(!isIntegerVertexFormat(VertexFormat::UByte4Norm));
    check(!isIntegerVertexFormat(VertexFormat::UInt1010102Norm));
};

auto tEveryFormatReadsItsValue = test("VertexFormat/everyFormatReadsItsValue") = []
{
    if (!Device::shared().isValid())
        return;

    auto makeLibrary = [](InputKind kind)
    {
        return Device::shared().makeShaderLibrary(
            nativeShaderSource(
                attributeMsl(kind), attributeHlsl(kind), attributeGlsl(kind))
                .withVertex("vertexMain")
                .withFragment("fragmentMain"));
    };

    auto floatLibrary = makeLibrary(InputKind::Float);
    auto uintLibrary = makeLibrary(InputKind::UInt);
    auto intLibrary = makeLibrary(InputKind::Int);

    for (const auto& entry: attributeCases())
    {
        check(isIntegerVertexFormat(entry.format) == (entry.kind != InputKind::Float));

        const auto& library = entry.kind == InputKind::Float  ? floatLibrary
                              : entry.kind == InputKind::UInt ? uintLibrary
                                                              : intLibrary;

        check(readAsExpected(drawAttribute(library, entry), entry));
    }
};
