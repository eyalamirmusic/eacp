#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/Sprites/Sprites.h>

#include <cstdint>
#include <optional>
#include <vector>

// Texture::update(region, ...) — filling one texture a tile at a time, which is
// how a glyph atlas is built: glyphs are rasterized as they are first needed and
// packed into a texture that already holds every glyph before them.
//
// A tile is added every tick. Only that tile's texels are sent to the GPU; the
// rest of the atlas is left alone. The whole-texture update() overload would
// re-send the entire atlas each time, and the running totals printed to the log
// show the difference that makes — with a 512x512 RGBA atlas and 16x16 tiles it
// is three orders of magnitude.
//
// Sampling is Nearest and the atlas is drawn magnified, so the texel grid stays
// visible and each upload is easy to see landing.

using namespace eacp;

namespace
{
constexpr auto atlasSize = 512;
constexpr auto tileSize = 16;
constexpr auto tilesPerRow = atlasSize / tileSize;
constexpr auto tileCount = tilesPerRow * tilesPerRow;

constexpr auto background = Graphics::Color {0.09f, 0.10f, 0.13f};
constexpr auto barBack = Graphics::Color {1.f, 1.f, 1.f, 0.10f};
constexpr auto barFill = Graphics::Color {0.40f, 0.75f, 0.55f};

std::uint32_t packRGBA(int r, int g, int b)
{
    return 0xff000000u | ((std::uint32_t) b << 16) | ((std::uint32_t) g << 8)
           | (std::uint32_t) r;
}

// Stands in for a rasterized glyph: a bordered block whose hue depends on the
// slot, so each upload is individually recognisable once it lands.
std::vector<std::uint32_t> makeTile(int index)
{
    auto pixels = std::vector<std::uint32_t> ((std::size_t) (tileSize * tileSize));

    const auto r = 60 + (index * 37) % 190;
    const auto g = 60 + (index * 71) % 190;
    const auto b = 90 + (index * 23) % 160;

    for (auto y = 0; y < tileSize; ++y)
    {
        for (auto x = 0; x < tileSize; ++x)
        {
            const auto edge = x == 0 || y == 0 || x == tileSize - 1
                              || y == tileSize - 1;

            pixels[(std::size_t) (y * tileSize + x)] =
                edge ? packRGBA(20, 22, 28) : packRGBA(r, g, b);
        }
    }

    return pixels;
}

GPU::Texture makeEmptyAtlas()
{
    const auto pixels =
        std::vector<std::uint32_t> ((std::size_t) (atlasSize * atlasSize),
                                    packRGBA(24, 26, 32));

    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = atlasSize;
    descriptor.height = atlasSize;
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;

    return GPU::Device::shared().makeTexture(descriptor, pixels.data());
}

struct AtlasView final : GPU::GPUView
{
    AtlasView()
        : atlas(makeEmptyAtlas())
    {
        setSampleCount(1);
    }

    void resized() override
    {
        GPUView::resized();

        const auto bounds = getLocalBounds();

        if (bounds.w > 0 && bounds.h > 0)
            sprites.emplace(Graphics::Point {bounds.w, bounds.h}, sampleCount());

        repaint();
    }

    void addNextTile()
    {
        if (nextTile >= tileCount)
            return;

        const auto column = nextTile % tilesPerRow;
        const auto row = nextTile / tilesPerRow;

        const auto tile = makeTile(nextTile);

        // The whole point: only this tile's texels cross to the GPU. Passing the
        // same pixels to update(pixels) would re-send all 1 MB of the atlas.
        atlas.update({(float) (column * tileSize),
                      (float) (row * tileSize),
                      (float) tileSize,
                      (float) tileSize},
                     tile.data());

        ++nextTile;

        constexpr auto tileBytes = tileSize * tileSize * 4;
        constexpr auto atlasBytes = atlasSize * atlasSize * 4;

        uploadedBytes += tileBytes;
        wholeTextureBytes += atlasBytes;

        // Logged rather than drawn: this example has no text renderer, and the
        // ratio is the thing worth taking away from it.
        if (nextTile % 32 == 0 || nextTile == tileCount)
            LOG(nextTile,
                "/",
                tileCount,
                " tiles — uploaded ",
                uploadedBytes / 1024,
                " KB by region vs ",
                wholeTextureBytes / 1024,
                " KB if each had re-sent the whole atlas");

        repaint();
    }

    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass({background});

        if (!sprites)
            return;

        sprites->begin(pass);

        const auto bounds = getLocalBounds();
        const auto margin = 24.f;
        const auto barHeight = 10.f;

        // Square, centred, as large as the window allows.
        const auto side = std::min(bounds.w - margin * 2.f,
                                   bounds.h - margin * 3.f - barHeight);
        const auto atlasRect = Graphics::Rect {(bounds.w - side) / 2.f,
                                               margin,
                                               side,
                                               side};

        // The default Nearest keeps the texel grid crisp under magnification, so
        // an upload is visible as a hard-edged block rather than a smear.
        sprites->drawTexture(atlas, atlasRect);

        const auto barRect = Graphics::Rect {atlasRect.x,
                                             atlasRect.y + side + margin / 2.f,
                                             side,
                                             barHeight};

        sprites->fillRect(barRect, barBack);
        sprites->fillRect({barRect.x,
                           barRect.y,
                           barRect.w * (float) nextTile / (float) tileCount,
                           barRect.h},
                          barFill);
    }

    GPU::Texture atlas;
    std::optional<Sprites::SpriteRenderer> sprites;

    int nextTile = 0;
    long long uploadedBytes = 0;
    long long wholeTextureBytes = 0;

    // A tile every 30ms: fast enough to watch fill, slow enough to see each one.
    Threads::Timer timer {[this] { addNextTile(); }, 33};
};

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};

    options.width = 640;
    options.height = 700;
    options.minWidth = 320;
    options.minHeight = 360;
    options.title = "Texture Atlas";
    options.backgroundColor = background;

    return options;
}

struct AtlasApp
{
    AtlasApp() { window.setContentView(view); }

    AtlasView view;
    Graphics::Window window {windowOptions()};
};
} // namespace

int main()
{
    return eacp::Apps::run<AtlasApp>();
}
