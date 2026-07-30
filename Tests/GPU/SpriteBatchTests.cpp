#include "Common.h"

#include <eacp/Sprites/Sprites.h>

#include <optional>

// What batching is allowed to change, and what it is not.
//
// SpriteRenderer queues quads and draws a run of them at once, which is only
// safe while the run is a *contiguous* span of the calls the caller made. The
// tempting optimisation - gather every quad sharing a texture and draw them
// together, however far apart they were issued - is wrong the moment two of them
// overlap, because with blending on, the order quads are drawn in is the
// picture. A batcher that reorders puts the wrong one on top, and nothing about
// the API says it happened.
//
// None of this has a CPU-side observable: the queue is drained into a command
// buffer and the only place the answer appears is the pixels. So these draw
// through a real pass and read them back.
//
// Self-skips without a GPU device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewW = 64.f;
constexpr auto viewH = 32.f;

// One texel each, so a draw is entirely about where the quad lands and never
// about how the texture is sampled.
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

// Runs one lambda's worth of drawing through a real pass at the view's size.
// Every test here differs only in what it draws, so the view itself is shared.
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
        // Outlives the pass deliberately - see CoordinateSpaceTests, where a
        // renderer built as a local released its pipeline while the command list
        // recording its draws was still waiting to be submitted.
        if (!sprites)
            sprites.emplace(Graphics::Point {viewW, viewH}, sampleCount());

        auto pass = frame.beginPass({Graphics::Color::black()});

        // No end() anywhere: the pass drains the renderer as it closes. That is
        // the contract, so the tests below are all drawn through it.
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
    // The reason the renderer joins the pass at all. Queued drawing is the
    // renderer's own business, so the pass ending has to be what collects it —
    // otherwise an app that never calls end() draws nothing whatsoever, and
    // nothing anywhere says so.
    //
    // Owned out here so it outlives the pass: a texture that dies with the
    // lambda would be gone by the time the drain reads it.
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

    // Red, then green, then red again, all over the same rect. The last one
    // issued is the one that should survive.
    //
    // This is the test that says runs are contiguous. Grouping by texture would
    // draw both reds and then the green, and the frame would come back green.
    auto image = renderDrawing(
        [whole](auto& sprites)
        {
            auto red = solidTexture(255, 0, 0);
            auto green = solidTexture(0, 255, 0);

            sprites.drawTexture(red, whole);
            sprites.drawTexture(green, whole);
            sprites.drawTexture(red, whole);

            // The textures are locals, so the queue has to be drained before
            // they go out of scope at the end of this lambda.
            sprites.end();
        });

    if (image.width() == 0)
        return;

    check(isRed(image.at(image.width() / 2, image.height() / 2)));
};

auto tEachQuadKeepsItsOwnTexture =
    test("SpriteBatch/eachQuadKeepsItsOwnTexture") = []
{
    // Green over everything, then red over the left half only. The right half is
    // the assertion that matters: it is covered by the green quad alone, so it
    // stays green only if the run was closed when the texture changed. A batcher
    // that let both quads into one run would draw them both with whichever
    // texture it happened to keep, and the whole window would come back one
    // colour.
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
    // One texture, many quads: exactly the case that collapses into a single
    // instanced draw. If only the first or last instance reached the pass, the
    // stripes between them would be missing.
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
    // The reason SpriteRenderer wraps setScissorRect at all. The red fill is
    // issued before the clip and must escape it; only the green one after it is
    // confined to the top strip. Setting the scissor straight on the pass would
    // clip both, because the red one is still queued at that point.
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
    // what a logical unit is worth on screen. A renderer built at the view's
    // size and then told it is half as wide should draw a quarter-width rect
    // across half the window - and do it without rebuilding anything, which is
    // the point of the setter.
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
    // to draw them first. Both fills below are a quarter of their own logical
    // width; if the queue were carried across the change, the first would be
    // re-measured against the new space and land in the wrong place.
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

    // The red one under the original mapping: the first quarter of the window.
    check(isRed(image.at(image.width() / 8, row)));

    // The green one under the halved mapping: its logical x of viewW/4 is half
    // way across, and it runs to the right edge.
    check(isGreen(image.at(image.width() * 3 / 4, row)));
};

// The regression gate for the per-flush allocation SpriteRenderer used to do.
//
// setInstances allocated a brand-new GPU::Buffer on every call, and the
// renderer calls it once per flush - on every texture change, and again at pass
// end. So a steady frame of drawing created several GPU resources, forever, in
// the frame loop. It was correct, because a buffer allocated a moment ago
// cannot be one the GPU is still reading, but it is exactly the churn the house
// rules forbid.
//
// Stated as a count rather than a timing, so it is deterministic, needs no
// clock, and cannot flake on a shared runner.
auto tSteadyStateAllocatesNoBuffers =
    test("SpriteBatch/steadyStateAllocatesNoBuffers") = []
{
    if (!Device::shared().isValid())
        return;

    auto red = solidTexture(255, 0, 0);
    auto green = solidTexture(0, 255, 0);

    // Alternating textures on purpose: a texture change is what ends a run, so
    // this is three flushes per frame rather than one. The bug being guarded
    // against scaled with flushes, not with frames.
    auto view = DrawingView {[&red, &green](auto& sprites)
                             {
                                 sprites.drawTexture(red, {0.f, 0.f, 16.f, 16.f});
                                 sprites.drawTexture(green, {20.f, 0.f, 16.f, 16.f});
                                 sprites.drawTexture(red, {40.f, 0.f, 16.f, 16.f});
                             }};

    // Warm: the first frame builds the pipeline and its library, and the pools
    // fill over the frames in flight after it.
    for (auto frame = 0; frame < 8; ++frame)
        view.renderToImage(1.f);

    const auto before = Device::shared().buffersCreated();

    for (auto frame = 0; frame < 10; ++frame)
        view.renderToImage(1.f);

    // Before StreamingBuffers this climbed by one per flush per frame.
    check(Device::shared().buffersCreated() == before);
};
