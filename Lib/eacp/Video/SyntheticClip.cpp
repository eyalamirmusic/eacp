#include "SyntheticClip.h"

#include "Encoder.h"

#include <algorithm>

namespace eacp::Video
{
namespace
{
// Saturated primaries and secondaries, plus black and white. Eight colours far
// enough apart in RGB that H.264's chroma subsampling and quantisation cannot
// turn one into another.
constexpr Graphics::Color clipPalette[] = {{1.0f, 0.0f, 0.0f},
                                           {0.0f, 1.0f, 0.0f},
                                           {0.0f, 0.0f, 1.0f},
                                           {1.0f, 1.0f, 0.0f},
                                           {1.0f, 0.0f, 1.0f},
                                           {0.0f, 1.0f, 1.0f},
                                           {1.0f, 1.0f, 1.0f},
                                           {0.1f, 0.1f, 0.1f}};

constexpr auto paletteSize = (int) (sizeof(clipPalette) / sizeof(clipPalette[0]));

// H.264 wants even dimensions; an odd one either fails to encode or is silently
// rounded, and a silently rounded size would then disagree with the decoded
// frames a test compares against.
int toEven(int value)
{
    return std::max(2, value - (value % 2));
}

// The pale bar sweeping across the middle third, which gives the clip motion to
// decode. Confined to the middle band vertically so the corners stay flat
// palette colour for a test to sample.
void drawSweep(Graphics::Image& image, int index, int frameCount)
{
    auto width = image.width();
    auto height = image.height();
    auto progress = frameCount > 1 ? (float) index / (float) (frameCount - 1) : 0.0f;

    auto barWidth = std::max(8, width / 12);
    auto barX = (int) (progress * (float) (width - barWidth));
    auto top = height / 3;
    auto bottom = height - height / 3;

    for (auto y = top; y < bottom; ++y)
        for (auto x = barX; x < barX + barWidth; ++x)
            image.set(x, y, Graphics::Color::gray(0.85f));
}
} // namespace

Graphics::Color syntheticFrameColor(int index)
{
    return clipPalette[((index % paletteSize) + paletteSize) % paletteSize];
}

int syntheticFrameCount(const SyntheticClipOptions& options)
{
    auto fps = std::max(1, options.fps);
    return std::max(1, (int) (options.duration * fps));
}

bool writeSyntheticClip(const FilePath& path, const SyntheticClipOptions& options)
{
    auto width = toEven(options.width);
    auto height = toEven(options.height);
    auto fps = std::max(1, options.fps);
    auto frameCount = syntheticFrameCount(options);

    // Roughly 0.1 bits per pixel per second, which is generous for flat colour
    // and keeps the encoder from inventing its own answer.
    auto bitrate = options.bitrate > 0 ? options.bitrate : width * height * fps / 10;

    auto encoder = makeEncoder();

    if (!encoder->begin(path, width, height, bitrate, fps))
        return false;

    auto image = Graphics::Image {width, height};

    for (auto index = 0; index < frameCount; ++index)
    {
        auto color = syntheticFrameColor(index);

        for (auto y = 0; y < height; ++y)
            for (auto x = 0; x < width; ++x)
                image.set(x, y, color);

        drawSweep(image, index, frameCount);

        // Offline: every frame has to land in the file, so wait rather than let
        // the encoder drop the ones it is too busy for.
        encoder->waitUntilReady(Time::MS {5000});
        encoder->appendImage(image, (double) index / fps);
    }

    // finish() finalises the file asynchronously and resolves on the main
    // thread, so this pumps the loop until the file is fully written.
    try
    {
        encoder->finish().waitFor(Time::MS {60'000});
    }
    catch (const Threads::AsyncError&)
    {
        return false;
    }

    return File {path}.exists();
}

FilePath cachedSyntheticClip(const SyntheticClipOptions& options)
{
    auto name = "eacp-synthetic-" + std::to_string(toEven(options.width)) + "x"
                + std::to_string(toEven(options.height)) + "-"
                + std::to_string(std::max(1, options.fps)) + "fps-"
                + std::to_string(syntheticFrameCount(options)) + ".mp4";

    auto path = FilePath::cacheDirectory() / name;

    if (File {path}.exists())
        return path;

    return writeSyntheticClip(path, options) ? path : FilePath {};
}
} // namespace eacp::Video
