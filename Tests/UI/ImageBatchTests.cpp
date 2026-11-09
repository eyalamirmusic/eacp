#include "Common.h"

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
// A solid image of one colour, `size` on a side.
eacp::Graphics::Image solid(int size, const Color& colour)
{
    auto image = eacp::Graphics::Image {size, size};

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            image.set(x, y, colour);

    return image;
}

bool isNearlyEqual(float a, float b, float tolerance = 1e-4f)
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
bool isImageColour(const Color& actual, const Color& expected)
{
    return isNearlyEqual(actual.r, expected.r, 0.02f)
           && isNearlyEqual(actual.g, expected.g, 0.02f)
           && isNearlyEqual(actual.b, expected.b, 0.02f);
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

// The pixels are hashed once per buffer, not once per ask. Shown by changing
// the bytes behind the image's back -- something no caller may do, and the one
// way to tell a lookup that reads no pixels from one that reads them all: the
// old content key would hash the new bytes and upload a second texture.
auto tHitReadsNoPixels =
    test("ImageCache/askingAgainForAnUnchangedImageReadsNoPixels") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto image = solid(4, red);

    auto first = cache.get(image);

    auto* bytes = const_cast<std::uint8_t*>(image.pixels().data());
    bytes[0] = 0;

    auto second = cache.get(image);

    check(first != nullptr);
    check(second == first, "found by the buffer, not by hashing it again");
    check(cache.size() == 1, "and nothing uploaded");
};

// A write through prepareForOverwrite keeps the buffer -- it is unshared and
// the size matches -- so the identity alone would find the old texture. The
// write count is what says these are other pixels.
auto tOverwriteInvalidates =
    test("ImageCache/anImageOverwrittenInPlaceIsUploadedAgain") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto image = solid(4, red);

    auto first = cache.get(image);
    const auto* before = image.pixels().data();

    auto* out = image.prepareForOverwrite(4, 4);

    for (auto i = 0; i < 4 * 4; ++i)
    {
        out[i * 4 + 0] = 0;
        out[i * 4 + 1] = 0;
        out[i * 4 + 2] = 255;
        out[i * 4 + 3] = 255;
    }

    check(out == before, "the same buffer, written in place");

    auto second = cache.get(image);

    check(second != nullptr);
    check(second != first, "the new pixels, not the texture of the old ones");
    check(second == cache.get(solid(4, blue)), "and found by their content");

    // And the same through set(), which writes in place too.
    image.set(0, 0, red);

    auto third = cache.get(image);

    check(third != second);
};

// A copy shares the buffer, so it is the same entry -- found by identity,
// without hashing the copy.
auto tCopySharesEntry = test("ImageCache/aCopyOfAnImageIsTheSameTexture") = []
{
    if (!hasDevice())
        return;

    auto cache = ImageCache {};
    auto image = solid(4, red);

    auto first = cache.get(image);
    auto copy = image;

    check(cache.get(copy) == first);
    check(cache.size() == 1);

    // Until the copy is written, which detaches it and leaves the original's
    // texture where it was.
    copy.set(0, 0, blue);

    auto detached = cache.get(copy);

    check(detached != first);
    check(cache.get(image) == first);
    check(cache.size() == 2);
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

    check(isNearlyEqual(uv.x, 0.25f));
    check(isNearlyEqual(uv.y, 0.5f));
    check(isNearlyEqual(uv.w, 0.5f));
    check(isNearlyEqual(uv.h, 0.25f));
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
    check(isImageColour(rendered.at(10.f, 20.f), red),
          "the left half is the red image");
    check(isImageColour(rendered.at(30.f, 20.f), {0.5f, 0.5f, 1.f, 1.f}),
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

    check(isImageColour(rendered.at(10.f, 20.f), red));
    check(isImageColour(rendered.at(30.f, 20.f), Color::white()), "cut by the clip");
};

auto tRadiiFitTheRect = test("RoundedRect/radiiPastTheRectShrinkTogether") = []
{
    auto uniform = fittedRadii({{0.f, 0.f, 40.f, 20.f}, CornerRadii::all(30.f)});

    check(isNearlyEqual(uniform.topLeft, 10.f), "half the shorter side");
    check(isNearlyEqual(uniform.bottomRight, 10.f));

    auto uneven = fittedRadii({{0.f, 0.f, 40.f, 40.f}, {30.f, 50.f, 0.f, 10.f}});

    check(isNearlyEqual(uneven.topLeft, 15.f), "the top asks for twice its length");
    check(isNearlyEqual(uneven.topRight, 25.f));
    check(isNearlyEqual(uneven.bottomLeft, 5.f));

    auto kept = fittedRadii({{0.f, 0.f, 40.f, 40.f}, {4.f, 0.f, 8.f, 2.f}});

    check(isNearlyEqual(kept.topLeft, 4.f) && isNearlyEqual(kept.bottomRight, 8.f),
          "radii that fit are left alone");
};

auto tImageIsCutByCorners = test("ImageBatch/anImageIsCutByItsCorners") = []
{
    if (!hasDevice())
        return;

    struct Rounded final : Component
    {
        void paint(UI::Graphics& g) override
        {
            auto image = getHost()->getImageCache().get(solid(2, red));
            auto bounds = getLocalBounds();
            auto box = RoundedRect {bounds, {16.f, 16.f, 16.f, 0.f}};
            auto source = Rect {0.f, 0.f, 2.f, 2.f};

            for (auto i = 0; i < 4; ++i)
                g.drawImage(image,
                            source,
                            {(float) i * 10.f, 0.f, 10.f, bounds.h},
                            box);
        }
    };

    auto host = ComponentHost {};
    auto rounded = Rounded {};

    host.setBackgroundColour(Color::white());
    host.setRootComponent(rounded);
    host.setBounds({0.f, 0.f, 40.f, 40.f});
    host.resized();

    auto rendered = Rendered {host};

    check(isImageColour(rendered.at(20.f, 20.f), red));
    check(isImageColour(rendered.at(1.f, 1.f), Color::white()),
          "the top-left corner is cut by the box, not by its tile");
    check(isImageColour(rendered.at(38.5f, 1.f), Color::white()));
    check(isImageColour(rendered.at(38.5f, 38.5f), Color::white()));
    check(isImageColour(rendered.at(1.f, 38.5f), red), "a square corner stays");
    check(isImageColour(rendered.at(5.f, 20.f), red), "the edge between corners");
    check(host.getLastImageDrawCount() == 1, "four tiles, one box, one draw");

    auto edge = rendered.at(16.f - 16.f / std::sqrt(2.f), 16.f - 16.f / std::sqrt(2.f));

    check(edge.g > 0.05f && edge.g < 0.95f, "anti-aliased on the curve");
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

namespace
{
eacp::Graphics::Image pattern(int size)
{
    auto image = eacp::Graphics::Image {size, size};

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
            image.set(x,
                      y,
                      Color {(float) x / (float) size,
                             (float) y / (float) size,
                             (float) ((x * 7 + y * 3) % 16) / 15.f,
                             1.f});

    return image;
}

ImageRef uploadedOnTheCpu(const eacp::Graphics::Image& image)
{
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = image.width();
    descriptor.height = image.height();
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;
    descriptor.mipmapped = true;

    return std::make_shared<ImageTexture>(descriptor, image.pixels().data(), 0);
}

struct Shown final : Component
{
    void paint(UI::Graphics& g) override { g.drawImage(image, area); }

    ImageRef image;
    Rect area;
};

eacp::Graphics::Image drawThrough(ComponentHost& host,
                                  Shown& shown,
                                  const ImageRef& image,
                                  float drawnPixels,
                                  float scale)
{
    shown.image = image;
    shown.area = {0.f, 0.f, drawnPixels / scale, drawnPixels / scale};
    shown.repaint();

    return host.renderToImage(scale);
}

int largestDifference(const eacp::Graphics::Image& a, const eacp::Graphics::Image& b)
{
    auto largest = 0;

    for (auto y = 0; y < a.height(); ++y)
    {
        for (auto x = 0; x < a.width(); ++x)
        {
            auto p = a.at(x, y);
            auto q = b.at(x, y);

            for (auto difference: {p.r - q.r, p.g - q.g, p.b - q.b, p.a - q.a})
                largest = std::max(largest,
                                   (int) std::lround(std::abs(difference) * 255.f));
        }
    }

    return largest;
}
} // namespace

auto tDeviceChainDrawsLikeCpu =
    test("ImageCache/aPictureDrawsTheSameWhicheverSideBuiltItsChain") = []
{
    if (!hasDevice())
        return;

    auto source = pattern(64);
    auto shown = Shown {};
    auto host = ComponentHost {};

    host.setBackgroundColour(Color::white());
    host.setRootComponent(shown);
    host.setBounds({0.f, 0.f, 64.f, 64.f});
    host.resized();

    auto cached = host.getImageCache().get(source);
    auto reference = uploadedOnTheCpu(source);

    check(cached->texture.mipLevels() == reference->texture.mipLevels());

    for (auto scale: {1.f, 2.f})
    {
        auto label = " at " + std::to_string((int) scale) + "x";

        auto full = drawThrough(host, shown, reference, 64.f, scale);
        auto fullCached = drawThrough(host, shown, cached, 64.f, scale);

        check(largestDifference(full, fullCached) == 0, "level 0" + label);

        auto small = drawThrough(host, shown, reference, 13.f, scale);
        auto smallCached = drawThrough(host, shown, cached, 13.f, scale);

        check(largestDifference(small, smallCached) <= 1,
              "minified, " + std::to_string(largestDifference(small, smallCached))
                  + " apart" + label);
    }
};

namespace
{
struct Cut final : Component
{
    void paint(UI::Graphics& g) override
    {
        auto image = getHost()->getImageCache().get(solid(2, red));

        g.drawImage(image, {0.f, 0.f, 2.f, 2.f}, area, corners);
    }

    Rect area;
    RoundedRect corners;
};

eacp::Graphics::Image drawCut(ComponentHost& host,
                              Cut& cut,
                              const RoundedRect& corners,
                              float scale)
{
    cut.corners = corners;
    cut.repaint();

    return host.renderToImage(scale);
}
} // namespace

// An uncut quad is cut by a square just past its edges, which has to read one
// at every pixel the quad touches. Multisampled, a pixel a sample of the quad
// covers shades at a centre outside it - at half a point per device pixel, an
// edge at 5.7 or 14.3 device pixels - so the edges are compared against the
// same quad cut by a square nowhere near it, which costs no alpha anywhere.
auto tUncutEdgesKeepTheirAlpha =
    test("ImageBatch/anUncutImageDrawsItsEdgesAsAFarCutDoes") = []
{
    if (!hasDevice() || !GPU::Device::shared().supportsSampleCount(4))
        return;

    auto cut = Cut {};
    auto host = ComponentHost {};

    cut.area = {11.4f, 11.4f, 17.2f, 17.2f};

    host.setSampleCount(4);
    host.setBackgroundColour(Color::white());
    host.setRootComponent(cut);
    host.setBounds({0.f, 0.f, 40.f, 40.f});
    host.resized();

    auto far = RoundedRect {{-20.f, -20.f, 80.f, 80.f}, CornerRadii::all(1.f)};

    for (auto scale: {0.5f, 1.f, 2.f})
    {
        auto uncut = drawCut(host, cut, {}, scale);
        auto reference = drawCut(host, cut, far, scale);

        check(largestDifference(uncut, reference) == 0,
              "at " + std::to_string(scale) + "x, "
                  + std::to_string(largestDifference(uncut, reference)) + " apart");
    }
};

// A rect with no radius is no cut, whether it is recorded or handed to the
// batch: a square cut is what the clip is for.
auto tZeroRadiiAreNoCut = test("ImageBatch/aRectWithNoRadiusDoesNotCut") = []
{
    if (!hasDevice())
        return;

    auto square = RoundedRect {{10.f, 10.f, 20.f, 20.f}};

    check(!square.isRounded());
    check(RoundedRect {square.rect, {0.f, 0.f, 2.f, 0.f}}.isRounded());
    check(!RoundedRect {{}, CornerRadii::all(4.f)}.isRounded(), "nor an empty one");

    auto cache = ImageCache {};
    auto image = cache.get(solid(2, red));
    auto recorder = Recorder {};

    recorder.g.drawImage(image, {0.f, 0.f, 2.f, 2.f}, {0.f, 0.f, 40.f, 40.f}, square);

    check(recorder.list.getImages()[0].corners.isEmpty(), "recorded uncut");

    auto cut = Cut {};
    auto host = ComponentHost {};

    cut.area = {0.f, 0.f, 40.f, 40.f};
    cut.corners = square;

    host.setBackgroundColour(Color::white());
    host.setRootComponent(cut);
    host.setBounds({0.f, 0.f, 40.f, 40.f});
    host.resized();

    auto rendered = Rendered {host};

    check(isImageColour(rendered.at(20.f, 20.f), red));
    check(isImageColour(rendered.at(5.f, 5.f), red), "drawn past the square");
};
