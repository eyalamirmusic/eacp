#include "Common.h"

#include <eacp/Sprites/Sprites.h>

#include <optional>

// A batched run is only safe while it stays a contiguous span of the calls the
// caller made: with blending on, the order quads are drawn in is the picture.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewW = 64.f;
constexpr auto viewH = 32.f;

// One texel each, so a draw is about where the quad lands and nothing else.
Texture solidTexture(unsigned char r, unsigned char g, unsigned char b)
{
    const unsigned char pixel[] = {r, g, b, 255};

    auto descriptor = TextureDescriptor {};
    descriptor.width = 1;
    descriptor.height = 1;
    descriptor.format = TextureFormat::RGBA8Unorm;

    return Device::shared().makeTexture(descriptor, pixel);
}

bool isRed(const Graphics::Color& c)
{
    return c.r > 0.5f && c.g < 0.5f && c.b < 0.5f;
}

bool isGreen(const Graphics::Color& c)
{
    return c.g > 0.5f && c.r < 0.5f && c.b < 0.5f;
}

bool isBlack(const Graphics::Color& c)
{
    return c.r < 0.2f && c.g < 0.2f && c.b < 0.2f;
}

struct DrawingView final : GPUView
{
    explicit DrawingView(std::function<void(Sprites::SpriteRenderer&)> drawToUse)
        : draw(std::move(drawToUse))
    {
        setSampleCount(1);
        setBounds({0.f, 0.f, viewW, viewH});
    }

    void render(Frame& frame) override
    {
        // Outlives the pass deliberately: a local would release its pipeline
        // while the command list recording its draws is still unsubmitted.
        if (!sprites)
            sprites.emplace(Graphics::Point {viewW, viewH}, sampleCount());

        auto pass = frame.beginPass({Graphics::Color::black()});

        // No end(): the pass drains the renderer as it closes.
        sprites->begin(pass);
        draw(*sprites);
    }

    std::function<void(Sprites::SpriteRenderer&)> draw;
    std::optional<Sprites::SpriteRenderer> sprites;
};

Graphics::Image renderDrawing(std::function<void(Sprites::SpriteRenderer&)> draw)
{
    auto view = DrawingView {std::move(draw)};
    return view.renderToImage(1.f);
}
} // namespace

auto tPassEndDrawsWithoutExplicitEnd =
    test("SpriteBatch/passEndDrawsWithoutExplicitEnd") = []
{
    // Owned out here so it outlives the pass: a texture dying with the lambda
    // would be gone by the time the drain reads it.
    auto green = solidTexture(0, 255, 0);

    auto image =
        renderDrawing([&green](auto& sprites)
                      { sprites.drawTexture(green, {0.f, 0.f, viewW, viewH}); });

    if (image.width() == 0)
        return;

    check(isGreen(image.at(image.width() / 2, image.height() / 2)));
};

auto tInterleavedTexturesKeepTheirOrder =
    test("SpriteBatch/interleavedTexturesKeepTheirOrder") = []
{
    const auto whole = Graphics::Rect {0.f, 0.f, viewW, viewH};

    // Grouping by texture would draw both reds and then the green, and the
    // frame would come back green.
    auto image = renderDrawing(
        [whole](auto& sprites)
        {
            auto red = solidTexture(255, 0, 0);
            auto green = solidTexture(0, 255, 0);

            sprites.drawTexture(red, whole);
            sprites.drawTexture(green, whole);
            sprites.drawTexture(red, whole);

            // The textures are locals: the queue must drain before they die.
            sprites.end();
        });

    if (image.width() == 0)
        return;

    check(isRed(image.at(image.width() / 2, image.height() / 2)));
};

auto tEachQuadKeepsItsOwnTexture =
    test("SpriteBatch/eachQuadKeepsItsOwnTexture") = []
{
    // The right half is covered by the green quad alone, so it stays green only
    // if the run was closed when the texture changed.
    auto image = renderDrawing(
        [](auto& sprites)
        {
            auto green = solidTexture(0, 255, 0);
            auto red = solidTexture(255, 0, 0);

            sprites.drawTexture(green, {0.f, 0.f, viewW, viewH});
            sprites.drawTexture(red, {0.f, 0.f, viewW / 2.f, viewH});

            sprites.end();
        });

    if (image.width() == 0)
        return;

    const auto row = image.height() / 2;

    check(isRed(image.at(image.width() / 4, row)));
    check(isGreen(image.at(image.width() * 3 / 4, row)));
};

auto tEveryQuadInARunIsDrawn = test("SpriteBatch/everyQuadInARunIsDrawn") = []
{
    // One texture, many quads: the case that collapses into one instanced draw.
    constexpr auto stripes = 8;
    constexpr auto stripeWidth = viewW / (float) stripes;

    auto image = renderDrawing(
        [](auto& sprites)
        {
            auto green = solidTexture(0, 255, 0);

            // Every other stripe, so a gap is as meaningful as a fill.
            for (auto i = 0; i < stripes; i += 2)
                sprites.drawTexture(
                    green, {(float) i * stripeWidth, 0.f, stripeWidth, viewH});

            sprites.end();
        });

    if (image.width() == 0)
        return;

    const auto row = image.height() / 2;
    const auto columnOf = [&image](int stripe)
    { return (int) (((float) stripe + 0.5f) * (float) image.width() / stripes); };

    for (auto i = 0; i < stripes; ++i)
    {
        const auto pixel = image.at(columnOf(i), row);

        if (i % 2 == 0)
            check(isGreen(pixel));
        else
            check(isBlack(pixel));
    }
};

auto tScissorDoesNotCatchEarlierQuads =
    test("SpriteBatch/scissorDoesNotCatchEarlierQuads") = []
{
    // The red fill is issued before the clip and must escape it: setting the
    // scissor straight on the pass would clip it too, as it is still queued.
    constexpr auto stripHeight = 8.f;

    auto image = renderDrawing(
        [](auto& sprites)
        {
            sprites.fillRect({0.f, 0.f, viewW, viewH},
                             Graphics::Color {1.f, 0.f, 0.f});

            sprites.setScissorRect({0.f, 0.f, viewW, stripHeight});
            sprites.fillRect({0.f, 0.f, viewW, viewH},
                             Graphics::Color {0.f, 1.f, 0.f});
            sprites.clearScissorRect();
        });

    if (image.width() == 0)
        return;

    const auto middle = image.width() / 2;

    check(isGreen(image.at(middle, 2)));
    check(isRed(image.at(middle, image.height() - 3)));
};

auto tLogicalSizeRemapsTheSpace = test("SpriteBatch/logicalSizeRemapsTheSpace") = []
{
    // The logical space always covers the whole target, so halving it doubles
    // what a logical unit is worth on screen.
    auto image = renderDrawing(
        [](auto& sprites)
        {
            sprites.setLogicalSize({viewW / 2.f, viewH});
            sprites.fillRect({0.f, 0.f, viewW / 4.f, viewH},
                             Graphics::Color {0.f, 1.f, 0.f});
        });

    if (image.width() == 0)
        return;

    const auto row = image.height() / 2;

    check(isGreen(image.at(image.width() / 4, row)));
    check(isBlack(image.at(image.width() * 3 / 4, row)));
};

auto tLogicalSizeChangeKeepsEarlierQuads =
    test("SpriteBatch/logicalSizeChangeKeepsEarlierQuads") = []
{
    // Quads issued before the change belong to the old space, so the change has
    // to draw them first.
    auto image = renderDrawing(
        [](auto& sprites)
        {
            sprites.fillRect({0.f, 0.f, viewW / 4.f, viewH},
                             Graphics::Color {1.f, 0.f, 0.f});

            sprites.setLogicalSize({viewW / 2.f, viewH});

            sprites.fillRect({viewW / 4.f, 0.f, viewW / 4.f, viewH},
                             Graphics::Color {0.f, 1.f, 0.f});
        });

    if (image.width() == 0)
        return;

    const auto row = image.height() / 2;

    check(isRed(image.at(image.width() / 8, row)));

    // Under the halved mapping a logical x of viewW/4 is half way across.
    check(isGreen(image.at(image.width() * 3 / 4, row)));
};

// Regression: setInstances allocated a new GPU::Buffer per flush, so a steady
// frame of drawing created several GPU resources, forever.
auto tSteadyStateAllocatesNoBuffers =
    test("SpriteBatch/steadyStateAllocatesNoBuffers") = []
{
    if (!Device::shared().isValid())
        return;

    auto red = solidTexture(255, 0, 0);
    auto green = solidTexture(0, 255, 0);

    // Alternating textures on purpose: a texture change ends a run, so this is
    // three flushes per frame rather than one.
    auto view = DrawingView {[&red, &green](auto& sprites)
                             {
                                 sprites.drawTexture(red, {0.f, 0.f, 16.f, 16.f});
                                 sprites.drawTexture(green, {20.f, 0.f, 16.f, 16.f});
                                 sprites.drawTexture(red, {40.f, 0.f, 16.f, 16.f});
                             }};

    // The first frame builds the pipeline, and the pools fill over the ones
    // after it.
    for (auto frame = 0; frame < 8; ++frame)
        view.renderToImage(1.f);

    const auto before = Device::shared().buffersCreated();

    for (auto frame = 0; frame < 10; ++frame)
        view.renderToImage(1.f);

    // Before StreamingBuffers this climbed by one per flush per frame.
    check(Device::shared().buffersCreated() == before);
};
