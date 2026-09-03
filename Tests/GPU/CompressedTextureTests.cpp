#include "Common.h"

#include <cstdint>
#include <cstring>

// TextureFormat's block-compressed members - BC1, BC2, BC3 and BC7 - and the
// thing about them nothing else in this suite can check: that the bytes a caller
// hands over reach the sampler unaltered, and that they reach it as the format
// the caller named.
//
// **Every block here is assembled by hand, field by field, out of the format's
// own specification rather than out of an encoder.** That is the whole point of
// the file. A block a compressor produced, compared against what that same
// compressor's decoder makes of it, checks nothing; a block whose 8 or 16 bytes
// are written out with the field boundaries in a comment beside them has an
// expected colour that can be derived on paper. Upload it at the wrong pitch,
// into the wrong subresource, or as the wrong DXGI/Metal format, and the colour
// that comes back is not the colour that was derived.
//
// The decoders are hardware and they round differently - the interpolated
// colours of a BC1 block are computed in 5:6:5 by some and at eight bits by
// others, which is a few levels apart - so the one case that reads an
// interpolated colour carries a tolerance and every case that reads an endpoint
// does not.
//
// Alpha is read by a shader that writes it into all three colour channels, not
// by blending onto a background, and the colour cases write an opaque alpha of
// their own. The snapshot read-back divides colour by alpha on its way out (see
// the note in BlendStateTests), so a fragment that kept the texture's own alpha
// would have the very number under test applied to it twice - and a BC3 block
// that is red at alpha 0 would read back as black rather than as red.
//
// Runs on both backends, and self-skips without a GPU device or on one with no
// BC formats at all - Device::supportsBlockCompression, which is the same
// refusal path an unsupported sampleCount takes.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct QuadVertex
{
    float position[2];
    float uv[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
// The whole texture across the whole viewport, uv running 0..1 left to right.
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

// Nearest on both, so a texel drawn 1:1 comes back as itself: these cases are
// equalities against a decode written down in advance, not ranges around a
// filtered neighbourhood.
//
// **The alpha it writes is its own, not the texture's**, which is the same
// constraint BlendStateTests spells out: the read-back un-premultiplies, so a
// fragment carrying a decoded alpha of 0 would have its colour divided away and
// every BC2 and BC3 case would be asserting on the alpha twice. The alpha is
// read by AlphaShader instead, where it is the value under test rather than a
// divisor.
struct ColorShader final : ShaderProgram
{
    ColorShader()
    {
        image.sampling = {TextureFilter::Nearest, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));

        auto texel = sample(image, varying(uv));
        setFragment(float4(texel.x(), texel.y(), texel.z(), 1.f));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

// The same draw with the texture's alpha written into r, g and b and an opaque
// alpha of its own - see the note at the top on why alpha is read this way.
struct AlphaShader final : ShaderProgram
{
    AlphaShader()
    {
        image.sampling = {TextureFilter::Nearest, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));

        auto texel = sample(image, varying(uv));
        setFragment(float4(texel.w(), texel.w(), texel.w(), 1.f));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

// One direction for the whole quad, so a face reads back as one colour rather
// than as a picture to interpret - the shape CubeTextureTests uses, and for the
// same reason.
struct CubeShader final : ShaderProgram
{
    CubeShader()
    {
        image.sampling = {TextureFilter::Nearest, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(sample(image, direction));
    }

    Uniform<TextureCube> image;
    Uniform<Float3> direction;

    EACP_SHADER(image, direction)
};

struct CubeView final : GPUView
{
    CubeView(Texture& cube, float x, float y, float z)
    {
        setSampleCount(1);

        shader.setVertices(fullQuad, 6);
        shader.image = cube;
        shader.direction = {x, y, z};
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.draw(shader);
    }

    CubeShader shader;
};

Graphics::Color sampleAlong(Texture& cube, float x, float y, float z)
{
    auto view = CubeView {cube, x, y, z};
    view.setBounds({0.f, 0.f, 4.f, 4.f});

    auto image = view.renderToImage(1.f);

    if (!image.isValid())
        return {};

    return image.at(2, 2);
}

template <typename Shader>
struct TextureView final : GPUView
{
    explicit TextureView(Texture& textureToShow)
    {
        setSampleCount(1);

        shader.setVertices(fullQuad, 6);
        shader.image = textureToShow;
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({{0.f, 0.f, 0.f, 1.f}});
        pass.draw(shader);
    }

    Shader shader;
};

template <typename Shader>
Graphics::Image draw(Texture& texture, int viewWidth, int viewHeight)
{
    auto view = TextureView<Shader> {texture};
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    return view.renderToImage(1.f);
}

bool isNear(float value, float target, float tolerance = 0.02f)
{
    return value > target - tolerance && value < target + tolerance;
}

bool isColor(const Graphics::Color& c, float r, float g, float b)
{
    // A tenth of the range: enough to separate the primaries these cases use
    // and to absorb whatever the drawable's own format does on the way back,
    // and far tighter than any decoder disagreement below.
    return isNear(c.r, r, 0.1f) && isNear(c.g, g, 0.1f) && isNear(c.b, b, 0.1f);
}

bool isRed(const Graphics::Color& c)
{
    return isColor(c, 1.f, 0.f, 0.f);
}
bool isBlue(const Graphics::Color& c)
{
    return isColor(c, 0.f, 0.f, 1.f);
}
bool isGreen(const Graphics::Color& c)
{
    return isColor(c, 0.f, 1.f, 0.f);
}

bool available()
{
    return Device::shared().isValid() && Device::shared().supportsBlockCompression();
}

// ---------------------------------------------------------------------------
// BC1, written out.
//
// Eight bytes: two 5:6:5 endpoints as little-endian uint16s, then 32 bits of
// two-bit indices - texel (x, y) at bits 2 * (4 * y + x), so byte 4 holds row 0
// with texel 0 in its lowest two bits.
//
// **The comparison of the two endpoints as unsigned integers selects the
// decode**, which is the one thing about BC1 that has to be got right and the
// one thing an encoder hides. color0 > color1 gives four opaque colours -
// color0, color1, and the thirds between them. color0 <= color1 gives three
// opaque colours and a fourth index that is *transparent black*, which is DXT1's
// punch-through alpha and the reason eacp needs no second format for it.
// ---------------------------------------------------------------------------

constexpr std::uint16_t red565 = 0xF800; // r = 31, g = 0, b = 0
constexpr std::uint16_t blue565 = 0x001F; // r = 0, g = 0, b = 31

// Every texel taking the same index, for a block that decodes to one colour.
constexpr std::uint32_t allIndices(std::uint32_t index)
{
    auto bits = std::uint32_t {0};

    for (auto texel = 0; texel < 16; ++texel)
        bits |= index << (texel * 2);

    return bits;
}

void writeBC1(std::uint8_t* block,
              std::uint16_t color0,
              std::uint16_t color1,
              std::uint32_t indices)
{
    block[0] = (std::uint8_t) (color0 & 0xff);
    block[1] = (std::uint8_t) (color0 >> 8);
    block[2] = (std::uint8_t) (color1 & 0xff);
    block[3] = (std::uint8_t) (color1 >> 8);

    for (auto byte = 0; byte < 4; ++byte)
        block[4 + byte] = (std::uint8_t) ((indices >> (byte * 8)) & 0xff);
}

Texture makeCompressed(TextureFormat format,
                       int width,
                       int height,
                       const std::uint8_t* blocks)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = format;

    return Device::shared().makeTexture(descriptor, blocks);
}
} // namespace

// The arithmetic first, which needs no device and cannot flake: a level of a
// compressed format is its size in whole 4x4 blocks, so every level below 4x4 is
// still one block and a 5-texel row is still two.
auto tBlockSizes = test("Compressed/levelSizesAreCountedInBlocks") = []
{
    check(isCompressedFormat(TextureFormat::BC1RGBA));
    check(isCompressedFormat(TextureFormat::BC7RGBA));
    check(!isCompressedFormat(TextureFormat::RGBA8Unorm));

    check(bytesPerBlock(TextureFormat::BC1RGBA) == 8);
    check(bytesPerBlock(TextureFormat::BC2RGBA) == 16);
    check(bytesPerBlock(TextureFormat::BC3RGBA) == 16);
    check(bytesPerBlock(TextureFormat::BC7RGBA) == 16);
    check(bytesPerBlock(TextureFormat::RGBA8Unorm) == 0);

    // No per-texel size at all, which is what stops it answering 4 for a format
    // whose texels are not four bytes each.
    check(bytesPerPixel(TextureFormat::BC1RGBA) == 0);
    check(bytesPerPixel(TextureFormat::RGBA8Unorm) == 4);

    check(levelBytesPerRow(TextureFormat::BC1RGBA, 8) == 16);
    check(levelBytesPerRow(TextureFormat::BC1RGBA, 5) == 16); // rounded up
    check(levelBytesPerRow(TextureFormat::BC1RGBA, 1) == 8);
    check(levelRows(TextureFormat::BC1RGBA, 8) == 2);
    check(levelRows(TextureFormat::BC1RGBA, 1) == 1);

    check(levelBytes(TextureFormat::BC1RGBA, 8, 8) == 32);
    check(levelBytes(TextureFormat::BC3RGBA, 8, 8) == 64);
    check(levelBytes(TextureFormat::BC1RGBA, 2, 2) == 8);
    check(levelBytes(TextureFormat::RGBA8Unorm, 8, 8) == 256);

    // 16x16 down to 1x1 in blocks: 16, 4, 1, 1, 1 of them.
    check(mipChainBytes(TextureFormat::BC1RGBA, 16, 16, 5) == 184);

    // And no chain eacp could build out of them, which is what makes
    // TextureDescriptor::mipLevels the only way a compressed texture gets one.
    check(!canBuildMipChain(TextureFormat::BC1RGBA));
    check(canBuildMipChain(TextureFormat::RGBA8Unorm));

    const std::uint8_t block[8] = {};
    check(!buildMipChain(block, 4, 4, TextureFormat::BC1RGBA).isValid());
};

// The device is asked rather than assumed - a texture in a format it refuses is
// invalid rather than quietly something else, exactly as a refused sampleCount
// is. Every Mac and every Direct3D device answers yes, so what can be pinned
// here is that the query exists and that this machine is one of them; the
// refusal itself runs the same lines Device::supportsSampleCount's does.
auto tDeviceAnswers = test("Compressed/theDeviceIsAskedForTheFormat") = []
{
    if (!Device::shared().isValid())
        return;

    check(Device::shared().supportsBlockCompression());
};

// **BC1's four-colour decode**, and the case the whole file rests on: one block,
// two endpoints a text editor can read, and three of the four indices checked
// against a colour derived from them rather than from a decoder.
//
// color0 = pure red and color1 = pure blue, with color0 > color1 as unsigned
// integers, so the block is in the four-colour mode: index 0 is red, 1 is blue,
// and 2 is two thirds of the way from red to blue. That last is the one number
// here a decoder can round differently - some interpolate in 5:6:5 and expand
// afterwards, which lands a few levels off - so it is the one with a tolerance.
auto tBC1Endpoints = test("Compressed/bc1DecodesItsEndpoints") = []
{
    if (!available())
        return;

    const std::uint32_t indices[] = {allIndices(0), allIndices(1), allIndices(2)};

    for (auto i = 0; i < 3; ++i)
    {
        std::uint8_t block[8];
        writeBC1(block, red565, blue565, indices[i]);

        auto texture = makeCompressed(TextureFormat::BC1RGBA, 4, 4, block);

        check(texture.isValid());

        if (!texture.isValid())
            return;

        const auto image = draw<ColorShader>(texture, 4, 4);

        if (!image.isValid())
            return;

        const auto pixel = image.at(2, 2);

        if (i == 0)
            check(isRed(pixel));
        else if (i == 1)
            check(isBlue(pixel));
        else
        {
            // (2 * color0 + color1) / 3, and a tolerance wide enough for a
            // decoder that computes it before expanding 5 bits to 8.
            check(isNear(pixel.r, 2.f / 3.f, 0.08f));
            check(isNear(pixel.b, 1.f / 3.f, 0.08f));
            check(pixel.g < 0.1f);
        }
    }
};

// **Two blocks side by side**, which is what pins block addressing and the row
// pitch a compressed upload is measured in. An 8x4 texture is two blocks and one
// block row, so its pitch is 16 bytes - the wrong pitch here puts the second
// block's bytes somewhere the sampler does not read, and the right half of the
// picture is not blue.
auto tBC1BlockAddressing = test("Compressed/bc1AddressesEachBlock") = []
{
    if (!available())
        return;

    std::uint8_t blocks[16];
    writeBC1(blocks, red565, blue565, allIndices(0)); // left block: red
    writeBC1(blocks + 8, red565, blue565, allIndices(1)); // right block: blue

    auto texture = makeCompressed(TextureFormat::BC1RGBA, 8, 4, blocks);

    check(texture.isValid());

    if (!texture.isValid())
        return;

    check(texture.mipLevels() == 1);

    const auto image = draw<ColorShader>(texture, 8, 4);

    if (!image.isValid())
        return;

    for (auto y = 0; y < image.height(); ++y)
    {
        for (auto x = 0; x < 4; ++x)
            check(isRed(image.at(x, y)));

        for (auto x = 4; x < 8; ++x)
            check(isBlue(image.at(x, y)));
    }
};

// **The punch-through alpha**, which is the half of BC1 that has no separate
// format and no flag anywhere: the *ordering of the two endpoints* is what says
// whether index 3 is a third colour or a transparent texel. The left block here
// has color0 > color1 and is opaque; the right has them the other way round with
// every index 3, and every texel of it is transparent black.
//
// A backend that took DXT1 and DXT1-with-alpha for two different things would
// have to choose one here and would fail one half of this case.
auto tBC1PunchThroughAlpha = test("Compressed/bc1PunchThroughAlpha") = []
{
    if (!available())
        return;

    std::uint8_t blocks[16];

    // Opaque: color0 > color1, so index 0 is simply color0.
    writeBC1(blocks, red565, blue565, allIndices(0));

    // Transparent: color0 <= color1 puts the block in the three-colour mode,
    // where index 3 is not a colour at all.
    writeBC1(blocks + 8, blue565, red565, allIndices(3));

    auto texture = makeCompressed(TextureFormat::BC1RGBA, 8, 4, blocks);

    check(texture.isValid());

    if (!texture.isValid())
        return;

    const auto alpha = draw<AlphaShader>(texture, 8, 4);
    const auto color = draw<ColorShader>(texture, 8, 4);

    if (!alpha.isValid() || !color.isValid())
        return;

    for (auto y = 0; y < alpha.height(); ++y)
    {
        for (auto x = 0; x < 4; ++x)
        {
            check(isNear(alpha.at(x, y).r, 1.f, 0.05f));
            check(isRed(color.at(x, y)));
        }

        for (auto x = 4; x < 8; ++x)
        {
            check(isNear(alpha.at(x, y).r, 0.f, 0.05f));

            // Transparent *black*: the colour goes with the alpha.
            check(isColor(color.at(x, y), 0.f, 0.f, 0.f));
        }
    }
};

// **BC2**, whose sixteen bytes are eight of explicit alpha and then a BC1 colour
// block - always read in the four-colour mode, the endpoint ordering having
// nothing left to say once alpha has its own eight bytes.
//
// The alpha half is four bits a texel, two texels a byte, **low nibble first**.
// So a row of 0x0f, 0x00 is texel 0 opaque and texels 1, 2 and 3 transparent -
// one column, not one half, and that is the point. A pattern of 0xff, 0x00 would
// say the same thing under either nibble convention, since both nibbles of every
// byte would agree; with only the low nibble of the first byte set, reading the
// nibbles the other way round moves the opaque column from x = 0 to x = 1.
//
// The pattern is the same in all four rows, so the case asserts on x alone -
// the y axis is not something the read-back pins, as TextureRegionTests says.
auto tBC2ExplicitAlpha = test("Compressed/bc2ExplicitAlpha") = []
{
    if (!available())
        return;

    std::uint8_t block[16];

    // Rows 0..3, two bytes each: texel 0 at 0xf, texels 1, 2 and 3 at 0x0.
    for (auto row = 0; row < 4; ++row)
    {
        block[row * 2] = 0x0f;
        block[row * 2 + 1] = 0x00;
    }

    writeBC1(block + 8, red565, blue565, allIndices(0));

    auto texture = makeCompressed(TextureFormat::BC2RGBA, 4, 4, block);

    check(texture.isValid());

    if (!texture.isValid())
        return;

    const auto alpha = draw<AlphaShader>(texture, 4, 4);
    const auto color = draw<ColorShader>(texture, 4, 4);

    if (!alpha.isValid() || !color.isValid())
        return;

    for (auto y = 0; y < alpha.height(); ++y)
    {
        check(isNear(alpha.at(0, y).r, 1.f, 0.05f));
        check(isNear(alpha.at(1, y).r, 0.f, 0.05f));
        check(isNear(alpha.at(2, y).r, 0.f, 0.05f));
        check(isNear(alpha.at(3, y).r, 0.f, 0.05f));

        // The colour block is the same red across the whole 4x4, which is what
        // says the alpha above is alpha and not a colour that happened to vary.
        for (auto x = 0; x < 4; ++x)
            check(isRed(color.at(x, y)));
    }
};

// **BC3**, whose alpha half is two endpoints and a three-bit index per texel
// rather than an explicit value. alpha0 > alpha1 selects the eight-value mode,
// where index 0 is alpha0 and index 1 is alpha1 - so with the endpoints at 255
// and 0 the two blocks below are fully opaque and fully transparent with no
// interpolation involved, and the numbers are exact.
//
// **The 48 index bits are one little-endian bit stream across bytes 2..7**,
// texel i at bits 3i - and the second block below is what pins that rather than
// merely assuming it. Setting every field to the same value would read the same
// under any field order at all, so the fields of column 0 are 0 (alpha0, opaque)
// and the other twelve are 1 (alpha1, transparent): 0x248248248248, which is
// 48 82 24 48 82 24 little-endian. Read the stream the other way round and the
// opaque column is not column 0.
//
// The pattern repeats every row, so this asserts on x alone - the y axis is not
// something the read-back pins.
auto tBC3InterpolatedAlpha = test("Compressed/bc3InterpolatedAlpha") = []
{
    if (!available())
        return;

    std::uint8_t blocks[32] = {};

    for (auto block = 0; block < 2; ++block)
    {
        auto* bytes = blocks + block * 16;

        bytes[0] = 255; // alpha0
        bytes[1] = 0; // alpha1, so alpha0 > alpha1: the eight-value mode

        if (block == 1)
        {
            // Column 0 of every row at index 0, the other three at index 1.
            const std::uint8_t indices[] = {0x48, 0x82, 0x24, 0x48, 0x82, 0x24};
            std::memcpy(bytes + 2, indices, sizeof(indices));
        }

        writeBC1(bytes + 8, red565, blue565, allIndices(0));
    }

    auto texture = makeCompressed(TextureFormat::BC3RGBA, 8, 4, blocks);

    check(texture.isValid());

    if (!texture.isValid())
        return;

    const auto alpha = draw<AlphaShader>(texture, 8, 4);
    const auto color = draw<ColorShader>(texture, 8, 4);

    if (!alpha.isValid() || !color.isValid())
        return;

    for (auto y = 0; y < alpha.height(); ++y)
    {
        // The whole of the left block is index 0, and so is column 0 of the
        // right one - which is the column that says the index stream was read
        // from the bottom bit up.
        for (auto x = 0; x < 5; ++x)
            check(isNear(alpha.at(x, y).r, 1.f, 0.05f));

        for (auto x = 5; x < 8; ++x)
            check(isNear(alpha.at(x, y).r, 0.f, 0.05f));

        // Both blocks carry the same colour, so what differs across the texture
        // is alpha alone.
        for (auto x = 0; x < 8; ++x)
            check(isRed(color.at(x, y)));
    }
};

// **BC7, in mode 6**, which is the one mode with a layout short enough to write
// out: one subset, RGBA endpoints of seven bits each with a p-bit apiece, and
// four-bit indices.
//
// Read from bit 0 upwards, a block is:
//
//     bits  0-6    mode, as m zeros then a one - so 6 is 0b1000000 = 0x40
//     bits  7-13   R0      bits 14-20   R1
//     bits 21-27   G0      bits 28-34   G1
//     bits 35-41   B0      bits 42-48   B1
//     bits 49-55   A0      bits 56-62   A1
//     bit  63      P0      bit  64      P1
//     bits 65-127  indices, four bits each, the first one bit shorter
//
// A channel decodes to (endpoint << 1) | p, so 127 with p = 1 is 255 and 0 with
// p = 1 is 1. Both endpoints of a block equal and every index 0 therefore give
// one flat colour, and the two blocks below are (255, 1, 1) and (1, 1, 255) -
// red and blue to within a level.
auto tBC7SolidBlocks = test("Compressed/bc7ModeSixSolidBlocks") = []
{
    if (!available())
        return;

    // R0 = R1 = 127, G = B = 0, A0 = A1 = 127, both p-bits set.
    const std::uint8_t redBlock[16] = {
        0xC0, 0xFF, 0x1F, 0x00, 0x00, 0x00, 0xFE, 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0};

    // The same with the 127s moved from R to B.
    const std::uint8_t blueBlock[16] = {
        0x40, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0};

    std::uint8_t blocks[32];
    std::memcpy(blocks, redBlock, 16);
    std::memcpy(blocks + 16, blueBlock, 16);

    auto texture = makeCompressed(TextureFormat::BC7RGBA, 8, 4, blocks);

    check(texture.isValid());

    if (!texture.isValid())
        return;

    const auto color = draw<ColorShader>(texture, 8, 4);
    const auto alpha = draw<AlphaShader>(texture, 8, 4);

    if (!color.isValid() || !alpha.isValid())
        return;

    for (auto y = 0; y < color.height(); ++y)
    {
        for (auto x = 0; x < 4; ++x)
            check(isRed(color.at(x, y)));

        for (auto x = 4; x < 8; ++x)
            check(isBlue(color.at(x, y)));

        // A0 = A1 = 127 with p = 1 is 255 everywhere, which says the alpha
        // channel of a mode-6 block was decoded rather than defaulted.
        for (auto x = 0; x < 8; ++x)
            check(isNear(alpha.at(x, y).r, 1.f, 0.05f));
    }
};

// A compressed texture is a block of bytes the sampler decodes and nothing else,
// so everything that would write into it a texel at a time is refused rather
// than half-supported - and refused at creation, which is what makes it an
// invalid texture rather than a bind that quietly does nothing.
auto tCompressedRefusals = test("Compressed/refusesWhatItCannotBe") = []
{
    if (!available())
        return;

    auto target = TextureDescriptor {};
    target.width = 4;
    target.height = 4;
    target.format = TextureFormat::BC1RGBA;
    target.renderTarget = true;

    check(!Device::shared().makeTexture(target, nullptr).isValid());

    auto kernelOutput = TextureDescriptor {};
    kernelOutput.width = 4;
    kernelOutput.height = 4;
    kernelOutput.format = TextureFormat::BC1RGBA;
    kernelOutput.computeWrite = true;

    check(!Device::shared().makeTexture(kernelOutput, nullptr).isValid());

    // And the format itself says so, which is what a caller checks before it
    // asks for either.
    check(!supportsComputeWrite(TextureFormat::BC1RGBA));
};

// mipmapped asks eacp's own filter for a chain, and that filter averages texels.
// A 4x4 block is not four numbers to take a mean of, so the texture gets exactly
// one level - not seven levels of which six were never written, which is what
// mipLevels() has always claimed and what a compressed format is the first
// format to actually test.
auto tCompressedMipmappedIsOneLevel = test("Compressed/mipmappedGivesOneLevel") = []
{
    if (!available())
        return;

    std::uint8_t blocks[16 * 8] = {};

    for (auto block = 0; block < 16; ++block)
        writeBC1(blocks + block * 8, red565, blue565, allIndices(0));

    auto descriptor = TextureDescriptor {};
    descriptor.width = 16;
    descriptor.height = 16;
    descriptor.format = TextureFormat::BC1RGBA;
    descriptor.mipmapped = true;

    auto texture = Device::shared().makeTexture(descriptor, blocks);

    check(texture.isValid());
    check(texture.mipLevels() == 1);

    // Still a usable texture, which is the half of "one level" that matters.
    const auto image = draw<ColorShader>(texture, 16, 16);

    if (!image.isValid())
        return;

    check(isRed(image.at(8, 8)));
};

// **A chain the compressor built**, which is the only chain a compressed texture
// can have and the reason TextureDescriptor::mipLevels exists at all. Level 0 is
// red and every level below it green, so a draw at full size reads level 0 and a
// draw at an eighth of it cannot.
//
// This is also the case that pins the per-level stride of a compressed chain: a
// level is levelBytes further on than the last, and levels below 4x4 are still
// one whole block each. Get that wrong and level 1 is read out of the middle of
// level 0's blocks, which decodes to neither colour.
auto tCompressedSuppliedChain = test("Compressed/aSuppliedChainMinifies") = []
{
    if (!available())
        return;

    constexpr auto size = 16;
    constexpr auto levels = 5; // 16, 8, 4, 2, 1

    auto bytes = Vector<std::uint8_t> {};
    bytes.resize((int) mipChainBytes(TextureFormat::BC1RGBA, size, size, levels));

    constexpr std::uint16_t green565 = 0x07E0;

    auto* at = bytes.data();

    for (auto level = 0; level < levels; ++level)
    {
        const auto levelWidth = mipExtent(size, level);
        const auto levelHeight = mipExtent(size, level);
        const auto levelSize =
            levelBytes(TextureFormat::BC1RGBA, levelWidth, levelHeight);

        for (auto block = 0; block < (int) (levelSize / 8); ++block)
            writeBC1(at + block * 8,
                     level == 0 ? red565 : green565,
                     blue565,
                     allIndices(0));

        at += levelSize;
    }

    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.format = TextureFormat::BC1RGBA;
    descriptor.mipLevels = levels;

    auto texture = Device::shared().makeTexture(descriptor, bytes.data());

    check(texture.isValid());
    check(texture.mipLevels() == levels);

    if (!texture.isValid())
        return;

    const auto full = draw<ColorShader>(texture, size, size);
    const auto minified = draw<ColorShader>(texture, 2, 2);

    if (!full.isValid() || !minified.isValid())
        return;

    check(isRed(full.at(size / 2, size / 2)));

    for (auto y = 0; y < minified.height(); ++y)
        for (auto x = 0; x < minified.width(); ++x)
            check(isGreen(minified.at(x, y)));
};

// Six faces of blocks, each face levelBytes on from the last - the same layout
// and the same order an uncompressed cube uses, which is the point: nothing
// about the six faces changes because the bytes inside one are compressed.
auto tCompressedCube = test("Compressed/aCubeOfBlocks") = []
{
    if (!available())
        return;

    const std::uint16_t faceColors[] = {
        red565,
        0x07E0, // green
        blue565,
        0xFFE0, // yellow
        0x07FF, // cyan
        0xF81F, // magenta
    };

    std::uint8_t faces[6 * 8];

    for (auto face = 0; face < 6; ++face)
        writeBC1(faces + face * 8, faceColors[face], blue565, allIndices(0));

    auto descriptor = TextureDescriptor {};
    descriptor.width = 4;
    descriptor.height = 4;
    descriptor.format = TextureFormat::BC1RGBA;
    descriptor.cube = true;

    auto cube = Device::shared().makeTexture(descriptor, faces);

    check(cube.isValid());
    check(cube.isCube());

    if (cube.isValid())
    {
        check(isColor(sampleAlong(cube, 1.f, 0.f, 0.f), 1.f, 0.f, 0.f));
        check(isColor(sampleAlong(cube, -1.f, 0.f, 0.f), 0.f, 1.f, 0.f));
        check(isColor(sampleAlong(cube, 0.f, 1.f, 0.f), 0.f, 0.f, 1.f));
        check(isColor(sampleAlong(cube, 0.f, -1.f, 0.f), 1.f, 1.f, 0.f));
        check(isColor(sampleAlong(cube, 0.f, 0.f, 1.f), 0.f, 1.f, 1.f));
        check(isColor(sampleAlong(cube, 0.f, 0.f, -1.f), 1.f, 0.f, 1.f));
    }

    // A supplied chain on a cube is the one combination refused: the faces would
    // each carry one, and nothing here can hold a cube's level selection still
    // long enough to say which level a direction read. See
    // TextureDescriptor::mipLevels.
    auto withChain = descriptor;
    withChain.mipLevels = 3;

    check(!Device::shared().makeTexture(withChain, faces).isValid());
};

// update() replaces the blocks of a compressed texture the same way it replaces
// the pixels of any other - the same tightly packed layout the constructor took.
// On D3D12 this is the path that has to move the resource back out of
// PIXEL_SHADER_RESOURCE before it can write, which the create-time upload never
// does.
//
// And bytesPerRow must be 0. A compressed layout is packed by definition, so a
// stride there is a number that can only be wrong; the second half of this case
// hands one over and checks that nothing moved.
auto tCompressedUpdate = test("Compressed/updateReplacesTheBlocks") = []
{
    if (!available())
        return;

    std::uint8_t blocks[16];
    writeBC1(blocks, red565, blue565, allIndices(0));
    writeBC1(blocks + 8, red565, blue565, allIndices(0));

    auto texture = makeCompressed(TextureFormat::BC1RGBA, 8, 4, blocks);

    check(texture.isValid());

    if (!texture.isValid())
        return;

    std::uint8_t replacement[16];
    writeBC1(replacement, red565, blue565, allIndices(1)); // blue
    writeBC1(replacement + 8, red565, blue565, allIndices(1)); // blue

    texture.update(replacement);

    {
        const auto image = draw<ColorShader>(texture, 8, 4);

        if (!image.isValid())
            return;

        check(isBlue(image.at(1, 1)));
        check(isBlue(image.at(6, 1)));
    }

    // A stride the layout cannot have: dropped, not used.
    std::uint8_t reds[16];
    writeBC1(reds, red565, blue565, allIndices(0));
    writeBC1(reds + 8, red565, blue565, allIndices(0));

    texture.update(reds, 16);

    {
        const auto image = draw<ColorShader>(texture, 8, 4);

        if (!image.isValid())
            return;

        check(isBlue(image.at(1, 1)));
    }
};

// The two entry points a compressed texture has no answer for. A region would
// have to land on the block grid, so the rect asked for and the rect written
// would differ by up to three texels a side; a read-back would have to hand back
// blocks in a layout nothing here consumes. Both are no-ops, and both are
// checked by their absence of effect rather than by a return value - neither has
// one.
auto tCompressedRegionAndRead = test("Compressed/regionAndReadAreNoOps") = []
{
    if (!available())
        return;

    std::uint8_t blocks[16];
    writeBC1(blocks, red565, blue565, allIndices(0));
    writeBC1(blocks + 8, red565, blue565, allIndices(0));

    auto texture = makeCompressed(TextureFormat::BC1RGBA, 8, 4, blocks);

    check(texture.isValid());

    if (!texture.isValid())
        return;

    std::uint8_t blue[8];
    writeBC1(blue, red565, blue565, allIndices(1));

    texture.update({0.f, 0.f, 4.f, 4.f}, blue);

    const auto image = draw<ColorShader>(texture, 8, 4);

    if (!image.isValid())
        return;

    check(isRed(image.at(1, 1)));

    // read() leaves the destination exactly as it found it.
    std::uint8_t destination[16];
    std::memset(destination, 0xCD, sizeof(destination));

    texture.read(destination);

    for (auto byte: destination)
        check(byte == 0xCD);
};
