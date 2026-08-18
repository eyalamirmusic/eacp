#include "Common.h"

#include <eacp/Video/Decode/FrameImage.h>
#include <eacp/Video/Demux/Mp4Demuxer.h>
#include <eacp/Video/SyntheticClip.h>

using namespace nano;
using namespace eacp;
using namespace VideoTests;

namespace
{
// Small and short: these tests are about the platform decoder agreeing with the
// platform encoder, not about throughput.
Video::SyntheticClipOptions testClipOptions()
{
    auto options = Video::SyntheticClipOptions {};
    options.width = 320;
    options.height = 240;
    options.fps = 10;
    options.duration = 1.6; // 16 frames, two full turns of the colour cycle
    return options;
}

// Encodes the clip once for the whole file rather than per test — it is the
// slow part, and every test here wants the same one.
const FilePath& testClip()
{
    static const auto path = Video::cachedSyntheticClip(testClipOptions());
    return path;
}

// A decoded frame's colour, read from the top-left corner. The sweeping bar is
// confined to the middle band, so the corner is always the flat frame colour —
// and being flat, it survives H.264 well enough to compare against.
//
// Goes through Video::toImage rather than frame.pixels(), because on the
// zero-copy backends a decoded frame carries a platform buffer and no CPU
// pixels at all; that is the whole reason toImage exists.
Graphics::Color cornerColor(const Video::VideoFrame& frame)
{
    auto image = Video::toImage(frame);
    return image.isValid() ? image.at(0, 0) : Graphics::Color {};
}

bool colorsMatch(const Graphics::Color& a, const Graphics::Color& b)
{
    // H.264 at this bitrate moves a flat field by a few levels at most; the
    // palette is far enough apart that a generous tolerance still identifies
    // the frame uniquely.
    constexpr auto tolerance = 0.15f;

    return std::abs(a.r - b.r) < tolerance && std::abs(a.g - b.g) < tolerance
           && std::abs(a.b - b.b) < tolerance;
}
} // namespace

// The encoder wrote a file the platform decoder can open, and it agrees about
// the dimensions and the duration.
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

// Every frame comes out, in presentation order, with rising timestamps.
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

// The decoded pixels are the ones that were encoded: frame N carries the colour
// writeSyntheticClip painted it with. This is the end-to-end check that the
// backend's pixel format, stride and row order are all right — get any of them
// wrong and the corner is the wrong colour.
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

// Seeking lands on the frame covering the requested time, which the frame's own
// colour proves rather than its timestamp.
auto tSeekLandsOnRightFrame = test("Decoder/seekLandsOnRequestedFrame") = []
{
    auto stream = Video::FrameStream {};
    check(stream.open(testClip()));

    // Frame 12 covers [1.2, 1.3).
    stream.seek(1.25);
    auto frame = stream.waitForFrameAt(1.25, waitTimeout);

    check(frame.isValid());
    check(colorsMatch(cornerColor(frame), Video::syntheticFrameColor(12)));

    // And back to a frame before it, which is the scrub-backwards path.
    stream.seek(0.35);
    frame = stream.waitForFrameAt(0.35, waitTimeout);

    check(frame.isValid());
    check(colorsMatch(cornerColor(frame), Video::syntheticFrameColor(3)));
};

// Opening a file that is not there fails rather than half-succeeding.
auto tMissingFile = test("Decoder/missingFileFailsToOpen") = []
{
    auto decoder = Video::makeDecoder();
    check(!decoder->open(FilePath::tempDirectory() / "eacp-no-such-clip.mp4"));

    auto stream = Video::FrameStream {};
    check(!stream.open(FilePath::tempDirectory() / "eacp-no-such-clip.mp4"));
    check(!stream.isOpen());
};

// What the platform encoder wrote when it was asked for sound as well: a real
// audio track, of the shape it was given and as long as the picture beside it.
// The container is the only witness that does not need an audio decoder.
auto tWritesAudioTrack = test("Encoder/writesAudioTrackBesideVideo") = []
{
    auto options = testClipOptions();
    options.audio = Video::AudioSpec {};

    auto path = Video::cachedSyntheticClip(options);
    check(!path.empty());

    auto demuxer = Video::Mp4Demuxer {};
    check(demuxer.open(path));

    auto& audio = demuxer.audioTrack();
    check(audio.present);
    check(audio.numChannels == options.audio->numChannels);
    check(audio.sampleRate == options.audio->sampleRate);
    check(std::abs(audio.seconds() - options.duration) < 0.1);
};

// The video-only clip the other tests use must stay video-only: a track that
// appears when nothing asked for one would put silence in every recording.
auto tWritesNoAudioTrackByDefault = test("Encoder/writesNoAudioTrackByDefault") = []
{
    auto demuxer = Video::Mp4Demuxer {};
    check(demuxer.open(testClip()));
    check(!demuxer.audioTrack().present);
};
