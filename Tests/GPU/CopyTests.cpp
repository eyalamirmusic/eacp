#include "Common.h"

#include <functional>

// Frame::copy and CommandBuffer::copy: texture to texture, buffer to buffer and
// between the two, recorded in order with the passes. Every texel a copy moves
// is distinct, so a copy from the wrong place, level or face reads back as the
// wrong value rather than as a plausible one.
//
// Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto size = 4;

constexpr std::uint32_t red = 0xff0000ff;
constexpr std::uint32_t green = 0xff00ff00;

struct QuadVertex
{
    float position[2];
    float uv[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}, {0.f, 1.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
    {{1.f, -1.f}, {1.f, 1.f}},
    {{1.f, 1.f}, {1.f, 0.f}},
    {{-1.f, 1.f}, {0.f, 0.f}},
};

struct SamplingShader final : ShaderProgram
{
    SamplingShader()
    {
        compile();
        setVertices(fullQuad, 6);
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = vertexInput(&QuadVertex::uv);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(sample(image, varying(uv)));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

// A distinct RGBA8 value per texel, per level and per face: the texel's index
// in the low byte, and the level and face above it.
std::uint32_t texelValue(int index, int level = 0, int face = 0)
{
    return 0xff000000u | (std::uint32_t) (face << 16) | (std::uint32_t) (level << 12)
           | (std::uint32_t) (index & 0xff) | ((std::uint32_t) index & 0xf00);
}

Vector<std::uint32_t> texels(int count, int level = 0, int face = 0)
{
    auto values = Vector<std::uint32_t> {};

    for (auto index = 0; index < count; ++index)
        values.add(texelValue(index, level, face));

    return values;
}

Texture makeTexture(const void* pixels, int width = size, bool renderTarget = false)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = width;
    descriptor.renderTarget = renderTarget;

    return Device::shared().makeTexture(descriptor, pixels);
}

Texture makeZeroTexture(int width = size)
{
    auto zeros = Vector<std::uint32_t> {};
    zeros.resize((std::size_t) (width * width));
    return makeTexture(zeros.data(), width);
}

Texture makeTarget()
{
    return makeTexture(nullptr, size, true);
}

Vector<std::uint32_t> readTexture(const Texture& texture)
{
    auto values = Vector<std::uint32_t> {};
    values.resize((std::size_t) (texture.width() * texture.height()));
    texture.read(values.data());
    return values;
}

Vector<std::uint32_t> readBuffer(const Buffer& buffer)
{
    auto values = Vector<std::uint32_t> {};
    values.resize((std::size_t) (buffer.size() / 4));
    buffer.read(values.data(), buffer.size());
    return values;
}

Vector<std::uint32_t> filled(int count, std::uint32_t value)
{
    auto values = Vector<std::uint32_t> {};

    for (auto index = 0; index < count; ++index)
        values.add(value);

    return values;
}

Buffer makeBuffer(const Vector<std::uint32_t>& values)
{
    return Device::shared().makeBuffer(
        values.data(), (std::int64_t) values.size() * 4, BufferUsage::Storage);
}

Buffer makeZeroBuffer(int words)
{
    auto zeros = Vector<std::uint32_t> {};
    zeros.resize((std::size_t) words);
    return makeBuffer(zeros);
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
    view.setBounds({0.f, 0.f, (float) size, (float) size});
    view.renderToImage(1.f);
}

// Reads one level (or face) of a texture through a texture-to-buffer copy on a
// command buffer of its own, since Texture::read reaches level 0 alone.
Vector<std::uint32_t> readLevel(const Texture& texture, int level, int slice = 0)
{
    const auto width = std::max(1, texture.width() >> level);
    auto buffer = makeZeroBuffer(width * width);
    auto commands = Device::shared().makeCommandBuffer();

    auto layout = BufferTextureCopy {};
    layout.level = level;
    layout.slice = slice;

    commands.copy(texture, buffer, layout);
    commands.commit();

    return readBuffer(buffer);
}

bool canCopy()
{
    return Device::shared().isValid();
}
} // namespace

auto tCopyWholeTexture = test("Copy/aWholeTextureArrivesWhole") = []
{
    if (!canCopy())
        return;

    const auto values = texels(size * size);
    auto source = makeTexture(values.data());
    auto destination = makeZeroTexture();

    runFrame([&](Frame& frame) { frame.copy(source, destination); });

    check(readTexture(destination) == values);
};

auto tCopySubRegion = test("Copy/aRegionLandsAtItsOrigin") = []
{
    if (!canCopy())
        return;

    const auto values = texels(size * size);
    auto source = makeTexture(values.data());
    auto destination = makeZeroTexture();

    auto region = TextureCopyRegion {};
    region.sourceOrigin = {1, 1};
    region.destinationOrigin = {0, 2};
    region.size = {2, 2};

    runFrame([&](Frame& frame) { frame.copy(source, destination, region); });

    const auto copied = readTexture(destination);

    for (auto y = 0; y < size; ++y)
    {
        for (auto x = 0; x < size; ++x)
        {
            const auto inside = x < 2 && y >= 2;
            const auto expected = inside ? values[(y - 2 + 1) * size + (x + 1)] : 0u;

            check(copied[y * size + x] == expected);
        }
    }
};

// A region whose size is left at zero runs to the edge of the source level.
auto tCopyDefaultSizeRunsToTheEdge = test("Copy/aZeroSizeRunsToTheEdge") = []
{
    if (!canCopy())
        return;

    const auto values = texels(size * size);
    auto source = makeTexture(values.data());
    auto destination = makeZeroTexture();

    auto region = TextureCopyRegion {};
    region.sourceOrigin = {2, 0};
    region.destinationOrigin = {2, 0};

    runFrame([&](Frame& frame) { frame.copy(source, destination, region); });

    const auto copied = readTexture(destination);

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            check(copied[y * size + x] == (x >= 2 ? values[y * size + x] : 0u));
};

// Level 1 of a supplied chain copied to level 1 of another, which keeps its own
// level 0 untouched.
auto tCopyMipLevel = test("Copy/levelOneToLevelOne") = []
{
    if (!canCopy())
        return;

    constexpr auto width = 8;
    constexpr auto levels = 3;

    auto chain = Vector<std::uint32_t> {};
    auto zeros = Vector<std::uint32_t> {};

    for (auto level = 0; level < levels; ++level)
    {
        const auto levelWidth = width >> level;

        for (auto value: texels(levelWidth * levelWidth, level))
        {
            chain.add(value);
            zeros.add(0u);
        }
    }

    auto describe = [&]
    {
        auto descriptor = TextureDescriptor {};
        descriptor.width = width;
        descriptor.height = width;
        descriptor.mipLevels = levels;
        return descriptor;
    };

    auto source = Device::shared().makeTexture(describe(), chain.data());
    auto destination = Device::shared().makeTexture(describe(), zeros.data());

    check(source.mipLevels() == levels);

    runFrame([&](Frame& frame)
             { frame.copy(source, destination, TextureCopyRegion::wholeLevel(1)); });

    check(readLevel(destination, 1) == texels(4 * 4, 1));
    check(readLevel(destination, 0) == filled(64, 0u));
    check(readLevel(destination, 2) == filled(4, 0u));
};

// Face 3 of one cube into face 1 of another: the slice on each side is the face.
auto tCopyCubeFace = test("Copy/aCubeFaceToAnotherFace") = []
{
    if (!canCopy())
        return;

    auto faces = Vector<std::uint32_t> {};
    auto zeros = Vector<std::uint32_t> {};

    for (auto face = 0; face < 6; ++face)
    {
        for (auto value: texels(size * size, 0, face))
        {
            faces.add(value);
            zeros.add(0u);
        }
    }

    auto describe = []
    {
        auto descriptor = TextureDescriptor {};
        descriptor.width = size;
        descriptor.height = size;
        descriptor.cube = true;
        return descriptor;
    };

    auto source = Device::shared().makeTexture(describe(), faces.data());
    auto destination = Device::shared().makeTexture(describe(), zeros.data());

    check(source.isCube());

    auto region = TextureCopyRegion {};
    region.sourceSlice = 3;
    region.destinationSlice = 1;

    runFrame([&](Frame& frame) { frame.copy(source, destination, region); });

    check(readLevel(destination, 0, 1) == texels(size * size, 0, 3));
    check(readLevel(destination, 0, 3) == filled(16, 0u));
};

auto tCopyBufferToBuffer = test("Copy/bufferToBufferAtOffsets") = []
{
    if (!canCopy())
        return;

    auto source = makeBuffer(texels(16));
    auto destination = makeZeroBuffer(16);

    runFrame([&](Frame& frame) { frame.copy(source, destination, 8, 20, 24); });

    const auto copied = readBuffer(destination);
    const auto values = texels(16);

    for (auto index = 0; index < 16; ++index)
    {
        const auto inside = index >= 5 && index < 11;
        check(copied[index] == (inside ? values[index - 3] : 0u));
    }
};

auto tCopyWholeBuffer = test("Copy/aWholeBufferToTheStartOfAnother") = []
{
    if (!canCopy())
        return;

    auto source = makeBuffer(texels(4));
    auto destination = makeZeroBuffer(8);

    runFrame([&](Frame& frame) { frame.copy(source, destination); });

    const auto copied = readBuffer(destination);

    for (auto index = 0; index < 8; ++index)
        check(copied[index] == (index < 4 ? texelValue(index) : 0u));
};

// Texels uploaded by a copy in the frame are what a later pass of the same
// frame samples.
auto tCopyBufferToTextureSampled = test("Copy/bufferToTextureIsSampledAfter") = []
{
    if (!canCopy())
        return;

    const auto values = texels(size * size);

    auto padded = Vector<std::uint32_t> {};

    for (auto y = 0; y < size; ++y)
    {
        for (auto x = 0; x < size; ++x)
            padded.add(values[y * size + x]);

        padded.add(0xdeadbeef);
        padded.add(0xdeadbeef);
    }

    auto source = makeBuffer(padded);
    auto uploaded = makeZeroTexture();
    auto target = makeTarget();

    auto reader = SamplingShader {};
    reader.image = uploaded;
    reader.prepare(1,
                   false,
                   PrimitiveTopology::Triangles,
                   BlendMode::None,
                   PixelFormat::RGBA8Unorm);

    runFrame(
        [&](Frame& frame)
        {
            auto layout = BufferTextureCopy {};
            layout.bytesPerRow = (size + 2) * 4;

            frame.copy(source, uploaded, layout);

            auto pass = frame.beginPass(target);
            pass.draw(reader);
        });

    check(readTexture(uploaded) == values);
    check(readTexture(target) == values);
};

// A region of a render target read back into a buffer with padded rows.
auto tCopyTextureToBuffer = test("Copy/textureToBufferWithAPitch") = []
{
    if (!canCopy())
        return;

    const auto values = texels(size * size);
    auto source = makeTexture(values.data());
    auto destination = makeZeroBuffer(3 * 4);

    auto layout = BufferTextureCopy {};
    layout.bufferOffset = 4;
    layout.bytesPerRow = 16;
    layout.origin = {1, 1};
    layout.size = {2, 2};

    runFrame([&](Frame& frame) { frame.copy(source, destination, layout); });

    const auto copied = readBuffer(destination);

    check(copied[0] == 0u);
    check(copied[1] == values[1 * size + 1]);
    check(copied[2] == values[1 * size + 2]);
    check(copied[5] == values[2 * size + 1]);
    check(copied[6] == values[2 * size + 2]);
    check(copied[3] == 0u);
    check(copied[4] == 0u);
};

// A copy between two passes sees the first and not the second: the target is
// red when it is copied and green after, and the copy keeps the red. A third
// pass sampling the copy sees red too.
auto tCopyOrderedWithPasses = test("Copy/aCopyBetweenPassesIsInOrder") = []
{
    if (!canCopy())
        return;

    auto drawn = makeTarget();
    auto copy = makeZeroTexture();
    auto sampled = makeTarget();

    auto reader = SamplingShader {};
    reader.image = copy;
    reader.prepare(1,
                   false,
                   PrimitiveTopology::Triangles,
                   BlendMode::None,
                   PixelFormat::RGBA8Unorm);

    runFrame(
        [&](Frame& frame)
        {
            frame.beginPass(drawn, {{1.f, 0.f, 0.f, 1.f}});
            frame.copy(drawn, copy);
            frame.beginPass(drawn, {{0.f, 1.f, 0.f, 1.f}});

            auto pass = frame.beginPass(sampled);
            pass.draw(reader);
        });

    check(readTexture(drawn) == filled(16, green));
    check(readTexture(copy) == filled(16, red));
    check(readTexture(sampled) == filled(16, red));
};

// The same copies on an off-screen command buffer.
auto tCopyOnACommandBuffer = test("Copy/onACommandBuffer") = []
{
    if (!canCopy())
        return;

    const auto values = texels(size * size);
    auto source = makeTexture(values.data());
    auto destination = makeZeroTexture();
    auto readBack = makeZeroBuffer(size * size);

    auto commands = Device::shared().makeCommandBuffer();
    commands.copy(source, destination);
    commands.copy(destination, readBack);
    commands.commit();

    check(readBuffer(readBack) == values);
    check(readTexture(destination) == values);
};

// Every refusal leaves the destination as it was.
auto tCopyRefusals = test("Copy/badCopiesAreRefused") = []
{
    if (!canCopy())
        return;

    const auto values = texels(size * size);
    auto source = makeTexture(values.data());
    auto destination = makeZeroTexture();
    auto larger = makeZeroTexture(size * 2);

    auto floatDescriptor = TextureDescriptor {};
    floatDescriptor.width = size;
    floatDescriptor.height = size;
    floatDescriptor.format = TextureFormat::R32Float;
    auto floats = Device::shared().makeTexture(floatDescriptor);

    auto buffer = makeBuffer(texels(8));
    auto bufferDestination = makeZeroBuffer(8);

    runFrame(
        [&](Frame& frame)
        {
            frame.copy(floats, destination);
            frame.copy(source, larger);

            auto outside = TextureCopyRegion {};
            outside.sourceOrigin = {3, 3};
            outside.size = {2, 2};
            frame.copy(source, destination, outside);

            frame.copy(source, destination, TextureCopyRegion::wholeLevel(1));

            auto face = TextureCopyRegion {};
            face.sourceSlice = 1;
            frame.copy(source, destination, face);

            auto deep = TextureCopyRegion {};
            deep.size = {2, 2, 2};
            frame.copy(source, destination, deep);

            frame.copy(source, source, TextureCopyRegion {});

            frame.copy(buffer, bufferDestination, 2, 0, 8);
            frame.copy(buffer, bufferDestination, 0, 0, 40);
            frame.copy(buffer, buffer, 0, 8, 16);
            frame.copy(buffer, bufferDestination, 0, 0, 0);

            auto shortRows = BufferTextureCopy {};
            shortRows.bytesPerRow = 8;
            frame.copy(buffer, destination, shortRows);

            auto pastTheEnd = BufferTextureCopy {};
            frame.copy(buffer, destination, pastTheEnd);

            auto offGrid = BufferTextureCopy {};
            offGrid.bufferOffset = 2;
            offGrid.size = {1, 1};
            frame.copy(buffer, destination, offGrid);
        });

    check(readTexture(destination) == filled(16, 0u));
    check(readBuffer(bufferDestination) == filled(8, 0u));
    check(readTexture(source) == values);
    check(readBuffer(buffer) == texels(8));
};
