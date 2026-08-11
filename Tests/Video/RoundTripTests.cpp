#include "Common.h"

#include <eacp/Video/Decode/FrameImage.h>
#include <eacp/Video/SyntheticClip.h>

using namespace nano;
using namespace eacp;
using namespace VideoTests;

namespace
{
Video::SyntheticClipOptions testClipOptions()
{
    auto options = Video::SyntheticClipOptions {};
    options.width = 320;
    options.height = 240;
    options.fps = 10;
    options.duration = 1.6; // 16 frames, two full turns of the colour cycle
    return options;
}

const FilePath& testClip()
{
    static const auto path = Video::cachedSyntheticClip(testClipOptions());
    return path;
}

// Read from the top-left corner, which the sweeping bar never touches. Via
// toImage because a zero-copy backend's frame carries no CPU pixels.
Graphics::Color cornerColor(const Video::VideoFrame& frame)
{
    auto image = Video::toImage(frame);
    return image.isValid() ? image.at(0, 0) : Graphics::Color {};
}

bool colorsMatch(const Graphics::Color& a, const Graphics::Color& b)
{
    // H.264 moves a flat field by a few levels at most, and the palette is far
    // enough apart that a generous tolerance still identifies the frame.
    constexpr auto tolerance = 0.15f;

    return std::abs(a.r - b.r) < tolerance && std::abs(a.g - b.g) < tolerance
           && std::abs(a.b - b.b) < tolerance;
}
} // namespace

auto tOpensEncodedFile = test("Decoder/opensEncodedFile") = []
{
    check(!testClip().empty());

    auto decoder = Video::makeDecoder();
    check(decoder->open(testClip()));

    auto info = decoder->info();
    check(info.width == 320);
    check(info.height == 240);
    check(std::abs(info.duration - 1.6) < 0.15);
};

auto tDecodesEveryFrame = test("Decoder/decodesEveryFrameInOrder") = []
{
    auto decoder = Video::makeDecoder();
    check(decoder->open(testClip()));

    auto frame = Video::VideoFrame {};
    auto count = 0;
    auto previous = -1.0;

    while (decoder->nextFrame(frame))
    {
        check(frame.seconds() > previous);
        check(frame.width() == 320);
        check(frame.duration() > 0.0);

        previous = frame.seconds();
        ++count;
    }

    check(count == Video::syntheticFrameCount(testClipOptions()));
};

auto tDecodesCorrectPixels = test("Decoder/decodesEncodedPixels") = []
{
    auto stream = Video::FrameStream {};
    check(stream.open(testClip()));

    for (auto index = 0; index < 8; ++index)
    {
        auto time = (index + 0.5) / 10.0;
        auto frame = stream.waitForFrameAt(time, waitTimeout);

        check(frame.isValid());
        check(colorsMatch(cornerColor(frame), Video::syntheticFrameColor(index)));
    }
};

auto tSeekLandsOnRightFrame = test("Decoder/seekLandsOnRequestedFrame") = []
{
    auto stream = Video::FrameStream {};
    check(stream.open(testClip()));

    // Frame 12 covers [1.2, 1.3).
    stream.seek(1.25);
    auto frame = stream.waitForFrameAt(1.25, waitTimeout);

    check(frame.isValid());
    check(colorsMatch(cornerColor(frame), Video::syntheticFrameColor(12)));

    stream.seek(0.35);
    frame = stream.waitForFrameAt(0.35, waitTimeout);

    check(frame.isValid());
    check(colorsMatch(cornerColor(frame), Video::syntheticFrameColor(3)));
};

auto tMissingFile = test("Decoder/missingFileFailsToOpen") = []
{
    auto decoder = Video::makeDecoder();
    check(!decoder->open(FilePath::tempDirectory() / "eacp-no-such-clip.mp4"));

    auto stream = Video::FrameStream {};
    check(!stream.open(FilePath::tempDirectory() / "eacp-no-such-clip.mp4"));
    check(!stream.isOpen());
};
