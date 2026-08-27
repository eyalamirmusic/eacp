#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <cmath>

// Pictures in the component tier: how a decoded image becomes a texture, what a
// paint() records for one, who keeps the texture alive, and what a frame draws.
//
// The ownership rule is the part worth testing, because nothing on screen
// reports it. A recording holds the texture it draws, so a list replayed after
// the component that painted it let the image go still draws something that
// exists; and the cache lets go of whatever nothing holds after each walk, so a
// tree that stopped drawing a picture is not paying for it.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
// The ramps own a texture and the glyph atlas owns two, so a painter needs a
// device -- but no window, no pass and no drawable.
bool hasDevice()
{
    return GPU::Device::shared().isValid();
}

const auto red = Color {1.f, 0.f, 0.f, 1.f};
const auto blue = Color {0.f, 0.f, 1.f, 1.f};

// A solid image of one colour, `size` on a side.
eacp::Graphics::Image solid(int size, const Color& colour)
{
    auto image = eacp::Graphics::Image {size, size};

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            image.set(x, y, colour);

    return image;
}

// A painter with nowhere to draw, which is all a recording needs.
struct Recorder
{
    explicit Recorder(const Rect& bounds = {0.f, 0.f, 100.f, 50.f})
        : g(list, ramps, text, bounds)
    {
    }

    DrawList list;
    GradientRamps ramps;
    Text::TextRenderer text;
    UI::Graphics g;
};

bool isNear(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

// A host rendered at its own scale, which is the scale its clips and glyphs
// are already in, and read back at a point rather than a pixel.
struct Rendered
{
    explicit Rendered(ComponentHost& host)
        : scale(host.backingScale())
        , image(host.renderToImage(scale))
    {
    }

    Color at(float x, float y) const
    {
        return image.at((int) (x * scale), (int) (y * scale));
    }

    float scale;
    eacp::Graphics::Image image;
};

// Within what eight bits and a linear filter allow.
bool isColour(const Color& actual, const Color& expected)
{
    return isNear(actual.r, expected.r, 0.02f) && isNear(actual.g, expected.g, 0.02f)
           && isNear(actual.b, expected.b, 0.02f);
}

// The left half red, the right half blue at half strength, out of two images
// the component never keeps: what it draws is what the recording holds.
struct Picture final : Component
{
    void paint(UI::Graphics& g) override
    {
        if (!drawsImages)
            return;

        auto& cache = getHost()->getImageCache();
        auto bounds = getLocalBounds();
        auto half = bounds.w * 0.5f;

        g.drawImage(cache.get(solid(2, red)), {0.f, 0.f, half, bounds.h});
        g.drawImage(cache.get(solid(2, blue)), {half, 0.f, half, bounds.h}, 0.5f);
    }

    bool drawsImages = true;
};

// A host with a picture in it, sized and told so by hand: a view in no window
// is never told, and that is where the host builds its renderers.
struct Scene
{
    Scene()
    {
        host.setBackgroundColour(Color::white());
        host.setRootComponent(picture);
        host.setBounds({0.f, 0.f, 40.f, 40.f});
        host.resized();
    }

    ComponentHost host;
    Picture picture;
};
} // namespace

auto tSamePixelsOneTexture = test("ImageCache/theSamePixelsAreOneTexture") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};

    auto first = cache.get(solid(4, red));
    auto second = cache.get(solid(4, red));

    check(first != nullptr);
    check(first == second, "found by content, whichever buffer it came in");
    check(cache.size() == 1);

    auto other = cache.get(solid(4, blue));

    check(other != nullptr);
    check(other != first);
    check(cache.size() == 2);

    check(cache.get(eacp::Graphics::Image {}) == nullptr, "nothing to upload");
};

auto tTextureHasItsChain = test("ImageCache/aTextureCarriesItsMipChain") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto image = cache.get(solid(8, red));

    check(image->width == 8);
    check(image->height == 8);
    check(image->texture.mipLevels() == 4, "8, 4, 2, 1");
};

auto tReleaseKeepsWhatIsHeld =
    test("ImageCache/releaseUnusedKeepsWhatSomebodyHolds") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto held = cache.get(solid(4, red));

    cache.releaseUnused();
    check(cache.size() == 1, "a caller has it");

    held.reset();
    cache.releaseUnused();
    check(cache.size() == 0, "and now nobody does");
};

auto tImageRecordsInOwnPoints = test("DrawList/anImageIsRecordedInItsOwnPoints") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto image = cache.get(solid(4, red));
    auto recorder = Recorder {};

    recorder.g.translate(10.f, 20.f);
    recorder.g.drawImage(image, {5.f, 5.f, 50.f, 40.f}, 0.5f);

    const auto& images = recorder.list.getImages();

    check(images.size() == 1);
    check(images[0].image == image);
    check(sameRect(images[0].bounds, {15.f, 25.f, 50.f, 40.f}), "origin applied");
    check(sameRect(images[0].uv, {0.f, 0.f, 1.f, 1.f}), "the whole of it");
    check(images[0].opacity == 0.5f);

    check(recorder.list.getCommands().size() == 1);
    check(recorder.list.getCommands()[0].kind == DrawCommand::Kind::Images);
};

auto tSourceRectIsUV =
    test("DrawList/aSourceRectIsRecordedAsTextureCoordinates") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto image = cache.get(solid(8, red));
    auto recorder = Recorder {};

    recorder.g.drawImage(image, {2.f, 4.f, 4.f, 2.f}, {0.f, 0.f, 10.f, 10.f});

    const auto& uv = recorder.list.getImages()[0].uv;

    check(isNear(uv.x, 0.25f));
    check(isNear(uv.y, 0.5f));
    check(isNear(uv.w, 0.5f));
    check(isNear(uv.h, 0.25f));
};

auto tNothingIsRecordedForNothing =
    test("DrawList/anImageDrawnAsNothingRecordsNothing") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto image = cache.get(solid(4, red));
    auto recorder = Recorder {};

    recorder.g.drawImage(nullptr, {0.f, 0.f, 10.f, 10.f});
    recorder.g.drawImage(image, {0.f, 0.f, 0.f, 10.f});
    recorder.g.drawImage(image, {0.f, 0.f, 10.f, 10.f}, 0.f);

    check(recorder.list.isEmpty());
};

auto tRunOfImagesIsOneCommand = test("DrawList/aRunOfImagesIsOneCommand") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto image = cache.get(solid(4, red));
    auto recorder = Recorder {};

    for (auto i = 0; i < 5; ++i)
        recorder.g.drawImage(image, {(float) i * 10.f, 0.f, 8.f, 8.f});

    check(recorder.list.getImages().size() == 5);
    check(recorder.list.getCommands().size() == 1);
    check(recorder.list.getCommands()[0].count == 5);

    // A shape between two images keeps the order they were issued in.
    recorder.g.fillRect({0.f, 20.f, 10.f, 10.f});
    recorder.g.drawImage(image, {0.f, 30.f, 8.f, 8.f});

    check(recorder.list.getCommands().size() == 3);
    check(recorder.list.getCommands()[1].kind == DrawCommand::Kind::Shapes);
    check(recorder.list.getCommands()[2].kind == DrawCommand::Kind::Images);
};

auto tRecordingHoldsTheTexture =
    test("DrawList/aRecordingKeepsTheTextureItDraws") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto recorder = Recorder {};

    {
        auto image = cache.get(solid(4, red));
        recorder.g.drawImage(image, {0.f, 0.f, 10.f, 10.f});
    }

    cache.releaseUnused();
    check(cache.size() == 1, "the list is holding it");

    recorder.list.clear();
    cache.releaseUnused();
    check(cache.size() == 0, "and let go with the list");
};

auto tFrameDrawsImages = test("ImageBatch/aFrameDrawsWhatTheTreeRecorded") = []
{
    if (!hasDevice())
        return;

    auto scene = Scene {};
    auto rendered = Rendered {scene.host};

    check(rendered.image.isValid());
    check(rendered.image.width() == (int) (40.f * rendered.scale));

    // The x axis carries the assertion: neither backend mirrors horizontally,
    // so nothing here assumes which way up the read-back arrives.
    check(isColour(rendered.at(10.f, 20.f), red), "the left half is the red image");
    check(isColour(rendered.at(30.f, 20.f), {0.5f, 0.5f, 1.f, 1.f}),
          "the right half is half blue over the white background");

    check(scene.host.getCachedImageCount() == 2);
    check(scene.host.getLastImageDrawCount() == 2, "two textures, two draws");
};

auto tRunOutOfOneImageIsOneDraw = test("ImageBatch/aRunOutOfOneImageIsOneDraw") = []
{
    if (!hasDevice())
        return;

    struct Row final : Component
    {
        void paint(UI::Graphics& g) override
        {
            auto image = getHost()->getImageCache().get(solid(2, red));

            for (auto i = 0; i < 8; ++i)
                g.drawImage(image, {(float) i * 5.f, 0.f, 4.f, 40.f});

            if (withShapeBetween)
            {
                g.setColour(blue);
                g.fillRect({0.f, 0.f, 1.f, 1.f});

                g.drawImage(image, {0.f, 38.f, 40.f, 2.f});
            }
        }

        bool withShapeBetween = false;
    };

    auto host = ComponentHost {};
    auto row = Row {};

    host.setRootComponent(row);
    host.setBounds({0.f, 0.f, 40.f, 40.f});
    host.resized();

    host.renderToImage(host.backingScale());
    check(host.getLastImageDrawCount() == 1, "eight quads, one texture, one draw");
    check(host.getLastRendererSwitchCount() == 0);

    row.withShapeBetween = true;
    row.repaint();

    host.renderToImage(host.backingScale());
    check(host.getLastImageDrawCount() == 2, "the shape broke the run");
    check(host.getLastRendererSwitchCount() == 2, "images, then quads, then images");
};

auto tImageIsCutByClip = test("ImageBatch/anImageIsCutByTheClip") = []
{
    if (!hasDevice())
        return;

    struct Clipped final : Component
    {
        void paint(UI::Graphics& g) override
        {
            auto bounds = getLocalBounds();

            g.reduceClipRegion({0.f, 0.f, bounds.w * 0.5f, bounds.h});
            g.drawImage(getHost()->getImageCache().get(solid(2, red)), bounds);
        }
    };

    auto host = ComponentHost {};
    auto clipped = Clipped {};

    host.setBackgroundColour(Color::white());
    host.setRootComponent(clipped);
    host.setBounds({0.f, 0.f, 40.f, 40.f});
    host.resized();

    auto rendered = Rendered {host};

    check(isColour(rendered.at(10.f, 20.f), red));
    check(isColour(rendered.at(30.f, 20.f), Color::white()), "cut by the clip");
};

auto tUnusedTextureGoes =
    test("ComponentHost/aTextureNothingDrawsIsDroppedAfterTheWalk") = []
{
    if (!hasDevice())
        return;

    auto scene = Scene {};

    scene.host.paintDirtyComponents();
    check(scene.host.getCachedImageCount() == 2, "held by the recording");

    scene.picture.drawsImages = false;
    scene.picture.repaint();
    scene.host.paintDirtyComponents();

    check(scene.host.getCachedImageCount() == 0,
          "the recording was cleared and painted without them");
};
