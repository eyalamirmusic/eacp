#include <eacp/Video/Decode/Decoder.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

// What PlayingHeavyContent cannot measure. That sample runs a real playback
// clock, so every rate it reports is pinned to the clip's frame rate by
// FrameStream's backpressure and every number is an answer to "does it keep
// up", never "by how much". This drives the Decoder directly: no queue, no
// clock, no renderer.
//
// Also times opening and seeking, which playback only ever pays once and so
// never shows.

using namespace eacp;

namespace
{
using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Everything open() does: parsing the container, creating a D3D11 device,
// instantiating the decoder and negotiating media types. Run twice, because the
// first one in a process also pays to load the graphics driver, and an app that
// opens clip after clip only pays that once.
OwningPointer<Video::Decoder> benchDecoderOpen(const FilePath& path,
                                               const char* label)
{
    auto start = Clock::now();

    auto decoder = Video::makeDecoder();
    auto opened = decoder->open(path);
    auto elapsed = millisSince(start);

    if (!opened)
    {
        std::printf("  Decoder::open %-5s FAILED (%.1f ms)\n", label, elapsed);
        return nullptr;
    }

    auto info = decoder->info();
    std::printf("  Decoder::open %-5s %8.1f ms   %dx%d  %.0f fps  %.1f s\n",
                label,
                elapsed,
                info.width,
                info.height,
                info.frameRate,
                info.duration);

    return decoder;
}

// Frames as fast as the decoder will produce them. This is the ceiling the
// playback benchmark hides.
void benchThroughput(Video::Decoder& decoder, int frames)
{
    auto frame = Video::VideoFrame {};
    auto decoded = 0;

    auto start = Clock::now();

    while (decoded < frames && decoder.nextFrame(frame))
        ++decoded;

    auto elapsed = millisSince(start);

    if (decoded == 0 || elapsed <= 0.0)
    {
        std::printf("  decode             no frames\n");
        return;
    }

    std::printf("  decode             %8.1f fps  (%d frames in %.2f s)\n",
                decoded * 1000.0 / elapsed,
                decoded,
                elapsed / 1000.0);
}

// Seek plus the first frame after it, which is the latency a scrub bar feels.
// Accurate mode decodes forward from the keyframe and discards, so it is the
// slower of the two by construction; the gap between them is the cost of that.
void benchSeek(Video::Decoder& decoder, Video::SeekMode mode, const char* label)
{
    auto duration = decoder.info().duration;

    if (duration <= 0.0)
    {
        std::printf("  seek %-9s     unknown duration\n", label);
        return;
    }

    constexpr auto seeks = 8;
    auto frame = Video::VideoFrame {};
    auto total = 0.0;
    auto worst = 0.0;

    for (auto i = 0; i < seeks; ++i)
    {
        // Spread across the file, avoiding both ends.
        auto target = duration * (0.1 + 0.8 * (double) i / (seeks - 1));

        auto start = Clock::now();
        decoder.seek(target, mode);
        decoder.nextFrame(frame);
        auto elapsed = millisSince(start);

        total += elapsed;
        worst = std::max(worst, elapsed);
    }

    std::printf("  seek %-9s     %8.1f ms mean   %.1f ms worst\n",
                label,
                total / seeks,
                worst);
}
} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::printf("usage: VideoBench <clip.mp4> [frames]\n");
        return 1;
    }

    auto path = FilePath {argv[1]};
    auto frames = argc > 2 ? std::atoi(argv[2]) : 300;

    std::printf("%s\n", argv[1]);

    benchDecoderOpen(path, "cold");
    auto decoder = benchDecoderOpen(path, "warm");

    if (decoder == nullptr)
        return 1;

    benchThroughput(*decoder, frames);
    benchSeek(*decoder, Video::SeekMode::Keyframe, "keyframe");
    benchSeek(*decoder, Video::SeekMode::Accurate, "accurate");

    std::printf("\n");
    return 0;
}
