#include "Common.h"

#include <cmath>

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
