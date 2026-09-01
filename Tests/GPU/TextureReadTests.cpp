#include "Common.h"

// Texture::read, and the one rule that makes it usable: what a frame drew is
// only there to be read once the frame has committed it.
//
// Three things are pinned here.
//
// That a texture read back is the texture that was uploaded, region and stride
// included - the cheap half, and the one that says the blit, the staging buffer
// and the row arithmetic line up on this backend.
//
// That a render target read back inside the frame that drew it, with a
// Frame::flush() in between, holds what the pass drew. This is what the whole
// pair exists for: a screenshot is taken from inside a frame, and without the
// flush it reads the frame before it. The check is deliberately against a
// colour nothing else in the test writes, so "the pass ran" and "the clear ran"
// cannot be confused.
//
// And that the frame carries on afterwards: the pass drawn *after* the flush
// lands in the target too, which is what says flush left a frame rather than
// ended one.
//
// Runs on both backends, and self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto targetSize = 4;

struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
// The left half of the target, so one draw can be told from another by where it
// landed rather than only by its colour.
constexpr QuadVertex leftHalf[] = {
    {{-1.f, -1.f}},
    {{0.f, -1.f}},
    {{-1.f, 1.f}},
    {{0.f, -1.f}},
    {{0.f, 1.f}},
    {{-1.f, 1.f}},
};

constexpr QuadVertex rightHalf[] = {
    {{0.f, -1.f}},
    {{1.f, -1.f}},
    {{0.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{0.f, 1.f}},
};

struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(color, 1.f));
    }

    Uniform<Float3> color;

    EACP_SHADER(color)
};

TextureDescriptor describeTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    return descriptor;
}

// Draws into a target, flushes, and reads the target back - all inside one
// frame, which is the case that has no other answer.
struct ReadBackView final : GPUView
{
    ReadBackView()
        : target(Device::shared().makeTexture(describeTarget()))
    {
        setSampleCount(1);

        auto prepare = [](FlatShader& shader, const QuadVertex(&quad)[6])
        {
            shader.setVertices(quad, 6);
            shader.prepare(1,
                           false,
                           PrimitiveTopology::Triangles,
                           BlendMode::None,
                           pixelFormatFor(TextureFormat::RGBA8Unorm));
        };

        prepare(first, leftHalf);
        prepare(second, rightHalf);

        first.color = {1.f, 0.f, 0.f};
        second.color = {0.f, 0.f, 1.f};
    }

    void render(Frame& frame) override
    {
        {
            auto into = frame.beginPass(target, {Graphics::Color::black()});
            into.draw(first);
        }

        frame.flush();
        target.read(afterFlush.data());

        {
            // Kept, not cleared: the second draw is added to what the read
            // above has already seen, so the final read says both halves.
            auto into = frame.beginPass(target, {Graphics::Color::black(), false});
            into.draw(second);
        }

        frame.flush();
        target.read(atEnd.data());

        // The drawable still has to be given a pass, or there is nothing to
        // present and the snapshot path has nothing to hand back.
        frame.beginPass({Graphics::Color::black()});
    }

    Texture target;
    FlatShader first;
    FlatShader second;

    static constexpr auto pixelCount = targetSize * targetSize;

    Array<unsigned char, pixelCount * 4> afterFlush {};
    Array<unsigned char, pixelCount * 4> atEnd {};
};

// The pixel at (x, y) of a read-back RGBA8 image, as four bytes.
const unsigned char* pixelAt(const unsigned char* pixels, int x, int y)
{
    return pixels + ((std::size_t) y * targetSize + (std::size_t) x) * 4;
}
} // namespace

// The cheap half: what update() put in is what read() takes out.
auto tReadReturnsWhatWasUploaded = test("TextureRead/returnsWhatWasUploaded") = []
{
    if (!Device::shared().isValid())
        return;

    auto pixels = Array<unsigned char, targetSize * targetSize * 4> {};

    for (auto i = 0; i < targetSize * targetSize; ++i)
    {
        pixels[i * 4 + 0] = (unsigned char) (i * 7);
        pixels[i * 4 + 1] = (unsigned char) (i * 3 + 1);
        pixels[i * 4 + 2] = (unsigned char) (255 - i * 5);
        pixels[i * 4 + 3] = 255;
    }

    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;

    auto texture = Device::shared().makeTexture(descriptor, pixels.data());

    check(texture.isValid());

    auto read = Array<unsigned char, targetSize * targetSize * 4> {};
    texture.read(read.data());

    check(read == pixels);
};

// One row out of the middle, at a stride wider than the row - the two things
// the whole-texture call cannot say anything about.
auto tReadHonoursRegionAndStride = test("TextureRead/honoursRegionAndStride") = []
{
    if (!Device::shared().isValid())
        return;

    auto pixels = Array<unsigned char, targetSize * targetSize * 4> {};

    for (auto i = 0; i < targetSize * targetSize; ++i)
    {
        pixels[i * 4 + 0] = (unsigned char) i;
        pixels[i * 4 + 3] = 255;
    }

    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;

    auto texture = Device::shared().makeTexture(descriptor, pixels.data());

    check(texture.isValid());

    constexpr auto stride = targetSize * 4 + 8;

    auto read = Array<unsigned char, stride * 2> {};
    read.fill(0xab);

    // Two rows of two texels, starting one in and one down.
    texture.read({1.f, 1.f, 2.f, 2.f}, read.data(), (std::size_t) stride);

    check(read[0] == (unsigned char) (targetSize + 1));
    check(read[4] == (unsigned char) (targetSize + 2));
    check(read[stride] == (unsigned char) (2 * targetSize + 1));
    check(read[stride + 4] == (unsigned char) (2 * targetSize + 2));

    // Past the region's own rows, the destination is untouched: the stride is
    // honoured rather than the rows being packed into it.
    check(read[stride - 1] == 0xab);
};

// A region that leaves the texture is dropped, not clamped - the same answer
// update() gives, and for the same reason.
auto tReadRefusesAnOutOfBoundsRegion =
    test("TextureRead/refusesAnOutOfBoundsRegion") = []
{
    if (!Device::shared().isValid())
        return;

    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;

    auto texture = Device::shared().makeTexture(descriptor);

    auto read = Array<unsigned char, targetSize * targetSize * 4> {};
    read.fill(0xab);

    texture.read({1.f, 1.f, (float) targetSize, (float) targetSize}, read.data());

    check(read[0] == 0xab);
};

// The one that matters: a target drawn into and read back inside the frame that
// drew it. Without Frame::flush() the read would see the frame before this one -
// here, nothing at all, since this is the first.
auto tFlushMakesTheFrameReadable =
    test("TextureRead/flushMakesTheFrameReadable") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = ReadBackView {};

    if (!view.target.isRenderTarget())
        return;

    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    view.renderToImage(1.f);

    // The first pass painted the left half red over a black clear.
    const auto* left = pixelAt(view.afterFlush.data(), 0, targetSize / 2);
    const auto* right =
        pixelAt(view.afterFlush.data(), targetSize - 1, targetSize / 2);

    check(left[0] > 200);
    check(left[2] < 55);

    check(right[0] < 55);
    check(right[2] < 55);

    // And the frame went on: the second pass, after the flush, put blue in the
    // half the first one left black.
    const auto* stillRed = pixelAt(view.atEnd.data(), 0, targetSize / 2);
    const auto* nowBlue = pixelAt(view.atEnd.data(), targetSize - 1, targetSize / 2);

    check(stillRed[0] > 200);
    check(nowBlue[2] > 200);
};
