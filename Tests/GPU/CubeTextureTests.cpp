#include "Common.h"

#include <cstdint>

// TextureDescriptor::cube - six faces, and the two things about them that
// nothing else can check.
//
// **Which face each direction reads**, which is the order the six arrive in, and
// **which way u and v run inside one**, which is how each face is oriented. Both
// are conventions rather than choices: Metal's cube slices, D3D12's array slices
// under a TEXTURECUBE view and OpenGL's GL_TEXTURE_CUBE_MAP_POSITIVE_X + i agree
// on both, which is what lets a cube assembled for any one of them be uploaded
// here untouched.
//
// They are worth pinning precisely because agreeing is what they are supposed to
// do. A face in the wrong slot, or flipped within its own slot, still samples
// and still looks like a picture - a sky is a sky either way round, and a
// reflection of the wrong wall is still a reflection. There is no error, no
// validation message and nothing on screen that says so, and the only place a
// mistake would show up is in a comparison against another renderer.
//
// Each case draws a quad covering the viewport, samples the cube along one
// direction held constant over the whole quad, and reads the pixels back - so
// what is being asserted on is what the sampler returned for that direction and
// nothing else.
//
// Runs on both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewWidth = 4;
constexpr auto viewHeight = 4;

// RGBA8, little-endian: A B G R.
constexpr std::uint32_t red = 0xff0000ff;
constexpr std::uint32_t green = 0xff00ff00;
constexpr std::uint32_t blue = 0xffff0000;
constexpr std::uint32_t yellow = 0xff00ffff;
constexpr std::uint32_t cyan = 0xffffff00;
constexpr std::uint32_t magenta = 0xffff00ff;

struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

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

// One direction for the whole quad, so every fragment reads the same texel and
// the read-back is one colour rather than a picture to interpret. The direction
// is a uniform rather than a vertex attribute for exactly that reason.
struct CubeShader final : ShaderProgram
{
    CubeShader()
    {
        // Nearest, so a direction aimed at a texel centre returns that texel and
        // not a blend of it with its neighbours - which is what makes the
        // orientation case an equality rather than a range.
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
    view.setBounds({0.f, 0.f, (float) viewWidth, (float) viewHeight});

    auto image = view.renderToImage(1.f);

    if (!image.isValid())
        return {};

    return image.at(viewWidth / 2, viewHeight / 2);
}

bool isColor(const Graphics::Color& c, std::uint32_t packed)
{
    const auto r = (float) (packed & 0xff) / 255.f;
    const auto g = (float) ((packed >> 8) & 0xff) / 255.f;
    const auto b = (float) ((packed >> 16) & 0xff) / 255.f;

    // A tenth of the range, which separates six primaries comfortably and still
    // allows for whatever the drawable's own format does on the way back.
    const auto near = [](float a, float bb)
    { return a > bb - 0.1f && a < bb + 0.1f; };

    return near(c.r, r) && near(c.g, g) && near(c.b, b);
}

// Six single-texel faces, one colour each, in the +X, -X, +Y, -Y, +Z, -Z order
// TextureDescriptor::cube declares. One texel per face is deliberate: it makes
// this case about the face order alone, with no orientation for it to be
// accidentally right or wrong about.
Texture makeSixColorCube(bool mipmapped = false)
{
    static std::uint32_t faces[] = {red, green, blue, yellow, cyan, magenta};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 1;
    descriptor.height = 1;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.cube = true;
    descriptor.mipmapped = mipmapped;

    return Device::shared().makeTexture(descriptor, faces);
}

// 2x2 faces. Everything is white except +Z, which carries four distinct colours
// - one per corner, row-major from the top-left, which is the layout every 2D
// texture here is uploaded in.
Texture makeOrientedCube()
{
    constexpr std::uint32_t white = 0xffffffff;

    static std::uint32_t faces[] = {
        white, white, white, white, // +X
        white, white, white, white, // -X
        white, white, white, white, // +Y
        white, white, white, white, // -Y
        red,   green, blue,  yellow, // +Z: TL, TR, BL, BR
        white, white, white, white, // -Z
    };

    auto descriptor = TextureDescriptor {};
    descriptor.width = 2;
    descriptor.height = 2;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.cube = true;

    return Device::shared().makeTexture(descriptor, faces);
}
} // namespace

// The baseline every case below rests on: a cube is created, says it is one, and
// a 2D texture of the same pixels says it is not. If a shader declaring a
// TextureCube is handed the second of those, neither backend reports it - so
// this is the only thing that can tell them apart.
auto tCubeSaysItIsOne = test("CubeTexture/aCubeKnowsItIsACube") = []
{
    if (!Device::shared().isValid())
        return;

    auto cube = makeSixColorCube();

    check(cube.isValid());
    check(cube.isCube());

    static std::uint32_t pixels[] = {red};

    auto flat = TextureDescriptor {};
    flat.width = 1;
    flat.height = 1;

    auto plain = Device::shared().makeTexture(flat, pixels);

    check(plain.isValid());
    check(!plain.isCube());
};

// Six faces of a rectangle is a shape neither API has, and a cube something on
// the GPU writes has no way to say which face it writes. Both are refused at
// creation rather than half-built, and both backends refuse them in the same
// words - see TextureDescriptor::cube.
auto tCubeRefusesWhatItCannotBe = test("CubeTexture/aCubeRefusesWhatItCannotBe") = []
{
    if (!Device::shared().isValid())
        return;

    static std::uint32_t pixels[24] = {};

    auto oblong = TextureDescriptor {};
    oblong.width = 2;
    oblong.height = 1;
    oblong.cube = true;

    check(!Device::shared().makeTexture(oblong, pixels).isValid());

    auto target = TextureDescriptor {};
    target.width = 2;
    target.height = 2;
    target.cube = true;
    target.renderTarget = true;

    check(!Device::shared().makeTexture(target, nullptr).isValid());
};

// **The face order.** Each axis reads the face that was uploaded at its index,
// and the whole of what this asserts is that the six landed where the header
// says they do. A backend that reversed a pair - +Y with -Y is the easy one to
// get wrong, the two APIs' documentation drawing their axes differently - passes
// every other case here and fails this one.
auto tEachAxisReadsItsFace = test("CubeTexture/eachAxisReadsItsOwnFace") = []
{
    if (!Device::shared().isValid())
        return;

    auto cube = makeSixColorCube();

    if (!cube.isValid())
        return;

    check(isColor(sampleAlong(cube, 1.f, 0.f, 0.f), red));
    check(isColor(sampleAlong(cube, -1.f, 0.f, 0.f), green));
    check(isColor(sampleAlong(cube, 0.f, 1.f, 0.f), blue));
    check(isColor(sampleAlong(cube, 0.f, -1.f, 0.f), yellow));
    check(isColor(sampleAlong(cube, 0.f, 0.f, 1.f), cyan));
    check(isColor(sampleAlong(cube, 0.f, 0.f, -1.f), magenta));
};

// A direction need not be a unit vector - which face is read is decided by the
// largest component's axis and sign, and where in it by the other two divided by
// that component. So a reflection vector goes in as the arithmetic produced it,
// and this is what says a normalize() in front of it would be wasted work.
auto tDirectionNeedNotBeUnit =
    test("CubeTexture/theDirectionNeedNotBeNormalized") = []
{
    if (!Device::shared().isValid())
        return;

    auto cube = makeSixColorCube();

    if (!cube.isValid())
        return;

    check(isColor(sampleAlong(cube, 40.f, 3.f, -2.f), red));
    check(isColor(sampleAlong(cube, 0.05f, 0.01f, -0.02f), red));
};

// **The orientation within a face**, which is the half of the convention the
// order test cannot see. For the +Z face the cube mapping is s from +x and t
// from -y, both scaled into [0, 1] - so the four directions below aim at the
// four texel centres of a 2x2 face, and what they must come back with is the
// four colours in the order the face was uploaded in: row 0 first, left to
// right.
//
// A face uploaded upside down swaps the two rows here and nothing else changes;
// a face uploaded mirrored swaps the two columns. Neither would be visible in
// anything but a comparison against another renderer, which is the reason for
// spending a case on it.
auto tFaceOrientation = test("CubeTexture/faceOrientationIsPinned") = []
{
    if (!Device::shared().isValid())
        return;

    auto cube = makeOrientedCube();

    if (!cube.isValid())
        return;

    check(isColor(sampleAlong(cube, -0.5f, 0.5f, 1.f), red)); // top left
    check(isColor(sampleAlong(cube, 0.5f, 0.5f, 1.f), green)); // top right
    check(isColor(sampleAlong(cube, -0.5f, -0.5f, 1.f), blue)); // bottom left
    check(isColor(sampleAlong(cube, 0.5f, -0.5f, 1.f), yellow)); // bottom right
};

// A chain is built per face out of that face's own pixels, so a cube whose faces
// are each one colour reads that colour at every level - and the face order
// survives the extra subresources, which is the thing that could break. On
// D3D12 the levels of one face are consecutive subresources and the faces follow
// one another, so an off-by-one in that arithmetic lands a face's level 0 on its
// neighbour's.
auto tMipmappedCube = test("CubeTexture/aMipmappedCubeKeepsItsFaces") = []
{
    if (!Device::shared().isValid())
        return;

    // Four texels a side, so the chain is three levels rather than the one a
    // single-texel face would have.
    constexpr auto size = 4;
    constexpr auto texels = size * size;

    static std::uint32_t faces[6 * texels] = {};
    const std::uint32_t colors[] = {red, green, blue, yellow, cyan, magenta};

    for (auto face = 0; face < 6; ++face)
        for (auto i = 0; i < texels; ++i)
            faces[face * texels + i] = colors[face];

    auto descriptor = TextureDescriptor {};
    descriptor.width = size;
    descriptor.height = size;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.cube = true;
    descriptor.mipmapped = true;

    auto cube = Device::shared().makeTexture(descriptor, faces);

    if (!cube.isValid())
        return;

    check(cube.mipLevels() == 3);

    check(isColor(sampleAlong(cube, 1.f, 0.f, 0.f), red));
    check(isColor(sampleAlong(cube, -1.f, 0.f, 0.f), green));
    check(isColor(sampleAlong(cube, 0.f, 1.f, 0.f), blue));
    check(isColor(sampleAlong(cube, 0.f, -1.f, 0.f), yellow));
    check(isColor(sampleAlong(cube, 0.f, 0.f, 1.f), cyan));
    check(isColor(sampleAlong(cube, 0.f, 0.f, -1.f), magenta));
};

// update() replaces all six faces, since there is no argument here that could
// name one - the same block of pixels the texture was created from. What this
// checks is that the second upload lands on the same six subresources the first
// one did, which on D3D12 is a different code path: the resource is being
// sampled by then and has to be moved back to COPY_DEST first.
auto tCubeUpdate = test("CubeTexture/updateReplacesAllSixFaces") = []
{
    if (!Device::shared().isValid())
        return;

    auto cube = makeSixColorCube();

    if (!cube.isValid())
        return;

    static std::uint32_t swapped[] = {magenta, cyan, yellow, blue, green, red};
    cube.update(swapped);

    check(isColor(sampleAlong(cube, 1.f, 0.f, 0.f), magenta));
    check(isColor(sampleAlong(cube, 0.f, 1.f, 0.f), yellow));
    check(isColor(sampleAlong(cube, 0.f, 0.f, -1.f), red));
};
