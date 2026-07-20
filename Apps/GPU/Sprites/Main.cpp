#include <eacp/Sprites/Sprites.h>

#include <cstdlib>

using namespace eacp;

namespace
{
constexpr int viewWidth = 800;
constexpr int viewHeight = 448;

// A procedural 16x16 sprite (a cyan diamond with a magenta rim on a transparent
// field) authored as a Graphics::Image, so the view exercises the Image -> GPU
// texture bridge - Device::makeTexture(const Image&) - alongside the renderer.
Graphics::Image makeSpriteImage()
{
    constexpr int size = 16;
    auto image = Graphics::Image(size, size);

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            const auto distance = std::abs(x - size / 2) + std::abs(y - size / 2);

            if (distance < size / 2)
                image.set(x, y, Graphics::Color {0.2f, 0.8f, 1.0f});
            else if (distance == size / 2)
                image.set(x, y, Graphics::Color {1.0f, 0.2f, 0.8f});
        }

    return image;
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = viewWidth;
    options.height = viewHeight;
    options.title = "eacp Sprites";
    options.flags = {Graphics::WindowFlags::Titled,
                     Graphics::WindowFlags::Closable,
                     Graphics::WindowFlags::Miniaturizable};
    return options;
}
} // namespace

// Exercises every SpriteRenderer entry point: a textured quad and its three
// flips, tinted source-rect crops, a translucent fill with an outline, and grid
// lines - all in the fixed logical pixel space.
struct SpritesView final : GPU::GPUView
{
    SpritesView()
    {
        setSampleCount(1);
        sprites.emplace(Graphics::Point {viewWidth, viewHeight}, sampleCount());
    }

    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.36f, 0.58f, 0.99f}});
        sprites->begin(pass);

        sprites->drawTexture(sprite, {100, 120, 64, 64});
        sprites->drawTexture(sprite, {180, 120, 64, 64}, true, false);
        sprites->drawTexture(sprite, {260, 120, 64, 64}, false, true);
        sprites->drawTexture(sprite, {340, 120, 64, 64}, true, true);

        for (auto i = 0; i < 4; ++i)
        {
            const auto column = i % 2;
            const auto row = i / 2;
            const auto fi = (float) i;

            const auto crop = Graphics::Rect {
                (float) column * 8.0f, (float) row * 8.0f, 8.0f, 8.0f};
            const auto tint =
                Graphics::Color {1.0f - fi * 0.2f, 0.4f + fi * 0.15f, 0.6f};
            sprites->drawTexture(
                sprite, crop, {440.0f + fi * 40.0f, 128.0f, 32.0f, 32.0f}, tint);
        }

        sprites->fillRect({80, 100, 540, 220}, {0.0f, 0.0f, 0.0f, 0.4f});
        sprites->drawRect({80, 100, 540, 220}, {1.0f, 1.0f, 1.0f, 0.9f}, 2.0f);

        const auto gridColor = Graphics::Color {1.0f, 1.0f, 1.0f, 0.15f};

        for (auto x = 0; x <= viewWidth; x += 32)
            sprites->drawLine({(float) x, 0.0f}, {(float) x, viewHeight}, gridColor);

        for (auto y = 0; y <= viewHeight; y += 32)
            sprites->drawLine({0.0f, (float) y}, {viewWidth, (float) y}, gridColor);

        // A thick diagonal: arbitrary orientation, only expressible now that the
        // renderer draws a rotated quad rather than axis-aligned pixel rows.
        sprites->drawLine({100, 300}, {600, 130}, {1.0f, 0.85f, 0.2f, 0.85f}, 4.0f);
    }

    std::optional<Sprites::SpriteRenderer> sprites;
    GPU::Texture sprite = GPU::Device::shared().makeTexture(makeSpriteImage());
};

struct SpritesApp
{
    SpritesApp() { window.setContentView(view); }

    SpritesView view;
    Graphics::Window window {windowOptions()};
};

int main()
{
    return eacp::Apps::run<SpritesApp>();
}
