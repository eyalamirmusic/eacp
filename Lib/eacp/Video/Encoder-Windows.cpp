#include <eacp/Core/Utils/WinInclude.h>

#include "Encoder.h"

#include <eacp/Core/Utils/Logging.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

// The Windows encoder: a Media Foundation IMFSinkWriter writing H.264 into an
// .mp4, with an AAC stream beside it when the spec asks for audio. Each
// snapshot frame is composited over black into a BGRA (RGB32) sample and
// written with a real-time presentation timestamp; the SinkWriter's implicit
// converter feeds the H.264 encoder, so playback runs at capture speed. The
// GpuDirect tier is not wired here yet (no D3D->MF zero-copy), so those hooks
// keep the base's "unsupported" default and callers fall back to Snapshot.

namespace eacp::Video
{
namespace
{
using Microsoft::WRL::ComPtr;

// One 100-nanosecond-tick duration for a frame at `fps`, the unit MF timestamps
// use.
LONGLONG frameDuration(int fps)
{
    return 10'000'000LL / (fps > 0 ? fps : 60);
}

LONGLONG toMediaTime(double seconds)
{
    return static_cast<LONGLONG>(std::llround(seconds * 1e7));
}

// The AAC encoder takes one of four byte rates and refuses everything else, so
// a requested bitrate is snapped to the nearest it will accept rather than
// failing the recording over it.
UINT32 aacBytesPerSecond(int bitrate)
{
    constexpr UINT32 supported[] = {12'000, 16'000, 20'000, 24'000};

    auto requested = static_cast<UINT32>(bitrate / 8);
    auto closest = supported[0];

    for (auto candidate: supported)
        if (std::abs((long) candidate - (long) requested)
            < std::abs((long) closest - (long) requested))
            closest = candidate;

    return closest;
}

// The encoder's other hard limits. Coercing these would record something the
// caller did not ask for, so they fail instead.
bool isEncodableAudio(const AudioSpec& spec)
{
    auto rateSupported = spec.sampleRate == 44'100 || spec.sampleRate == 48'000;
    auto channelsSupported = spec.numChannels == 1 || spec.numChannels == 2;

    if (!rateSupported || !channelsSupported)
        LOG("VideoRecorder: the AAC encoder takes 44100 or 48000 Hz, mono or "
            "stereo");

    return rateSupported && channelsSupported;
}

// Media Foundation calls need COM up on the calling thread, and the recorder
// drains audio on a thread this encoder never created. Balanced at thread exit.
struct ComOnThisThread
{
    ComOnThisThread()
        : initialized(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
    }

    ~ComOnThisThread()
    {
        if (initialized)
            CoUninitialize();
    }

    bool initialized = false;
};

struct WindowsEncoder final : Encoder
{
    ~WindowsEncoder() override
    {
        writer.Reset();

        if (mfStarted)
            MFShutdown();

        if (comInitialized)
            CoUninitialize();
    }

    bool begin(const FilePath& path, const EncoderSpec& spec) override
    {
        if (spec.audio && !isEncodableAudio(*spec.audio))
            return false;

        width = spec.video.width;
        height = spec.video.height;
        fps = spec.video.fps > 0 ? spec.video.fps : 60;
        audioSpec = spec.audio;

        // MF needs COM up on this thread. The GUI thread is usually already
        // apartment-initialized; a matching re-init returns S_FALSE (still ours
        // to balance), only a conflicting mode fails -- then we do not un-init.
        auto comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        comInitialized = SUCCEEDED(comResult);

        if (FAILED(MFStartup(MF_VERSION)))
            return false;
        mfStarted = true;

        // FilePath carries UTF-8; Media Foundation wants a wide URL.
        auto url = path.wide();
        DeleteFileW(url.c_str());

        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(MFCreateAttributes(&attributes, 2)))
        {
            // We pace frames ourselves, so let the writer accept them as fast as
            // they arrive, and allow a hardware encoder when one is present.
            attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
            attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }

        if (FAILED(MFCreateSinkWriterFromURL(
                url.c_str(), nullptr, attributes.Get(), &writer)))
            return false;

        if (!configureStreams(spec))
        {
            writer.Reset();
            return false;
        }

        return SUCCEEDED(writer->BeginWriting());
    }

    bool configureStreams(const EncoderSpec& spec)
    {
        auto bitrate = spec.video.bitrate;
        ComPtr<IMFMediaType> outputType;
        if (FAILED(MFCreateMediaType(&outputType)))
            return false;

        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outputType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate));
        outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(outputType.Get(),
                           MF_MT_FRAME_SIZE,
                           static_cast<UINT32>(width),
                           static_cast<UINT32>(height));
        MFSetAttributeRatio(
            outputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
        MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (FAILED(writer->AddStream(outputType.Get(), &streamIndex)))
            return false;

        // BGRA input, top-down (positive stride): the SinkWriter inserts the
        // colour converter that feeds the H.264 encoder. Matches the straight
        // premultiplied-over-black BGRA compositeOverBlackBGRA writes.
        ComPtr<IMFMediaType> inputType;
        if (FAILED(MFCreateMediaType(&inputType)))
            return false;

        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width * 4));
        MFSetAttributeSize(inputType.Get(),
                           MF_MT_FRAME_SIZE,
                           static_cast<UINT32>(width),
                           static_cast<UINT32>(height));
        MFSetAttributeRatio(
            inputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
        MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (FAILED(writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr)))
            return false;

        return !spec.audio || configureAudioStream(*spec.audio);
    }

    // 16-bit PCM in, AAC out: the sink writer inserts the encoder between them.
    // Float would need a converter the AAC MFT does not accept in front of it,
    // so appendAudio does the one conversion by hand.
    bool configureAudioStream(const AudioSpec& spec)
    {
        auto channels = static_cast<UINT32>(spec.numChannels);
        auto rate = static_cast<UINT32>(spec.sampleRate);
        auto blockAlign = channels * 2;

        ComPtr<IMFMediaType> outputType;
        if (FAILED(MFCreateMediaType(&outputType)))
            return false;

        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                              aacBytesPerSecond(audioBitrateFor(spec)));

        if (FAILED(writer->AddStream(outputType.Get(), &audioStreamIndex)))
            return false;

        ComPtr<IMFMediaType> inputType;
        if (FAILED(MFCreateMediaType(&inputType)))
            return false;

        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
        inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * blockAlign);
        inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        return SUCCEEDED(
            writer->SetInputMediaType(audioStreamIndex, inputType.Get(), nullptr));
    }

    void appendImage(const Graphics::Image& image, double ptsSeconds) override
    {
        if (!writer)
            return;

        auto sizeInBytes = static_cast<DWORD>(width * height * 4);

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(sizeInBytes, &buffer)))
            return;

        BYTE* data = nullptr;
        DWORD maxLength = 0;
        if (FAILED(buffer->Lock(&data, &maxLength, nullptr)))
            return;

        compositeOverBlackBGRA(
            image, data, width, height, static_cast<std::size_t>(width) * 4);

        buffer->Unlock();
        buffer->SetCurrentLength(sizeInBytes);

        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample)))
            return;

        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(toMediaTime(ptsSeconds));
        sample->SetSampleDuration(frameDuration(fps));

        auto lock = std::lock_guard {writeMutex};
        writer->WriteSample(streamIndex, sample.Get());
    }

    bool acceptsAudio() const override { return writer && audioSpec.has_value(); }

    void appendAudio(const AudioBuffer& buffer, double ptsSeconds) override
    {
        thread_local auto com = ComOnThisThread {};

        if (!writer || !audioSpec || !buffer.isValid())
            return;

        auto channels = audioSpec->numChannels;
        auto frames = buffer.numFrames;
        auto sizeInBytes = static_cast<DWORD>(frames * channels * 2);

        ComPtr<IMFMediaBuffer> mediaBuffer;
        if (FAILED(MFCreateMemoryBuffer(sizeInBytes, &mediaBuffer)))
            return;

        BYTE* data = nullptr;
        DWORD maxLength = 0;
        if (FAILED(mediaBuffer->Lock(&data, &maxLength, nullptr)))
            return;

        auto* samples = reinterpret_cast<std::int16_t*>(data);

        for (auto channel = 0; channel < channels; ++channel)
        {
            auto source = buffer.channel(std::min(channel, buffer.numChannels - 1));

            for (auto frame = 0; frame < frames; ++frame)
            {
                auto value = std::clamp(source[frame], -1.f, 1.f);
                samples[frame * channels + channel] =
                    static_cast<std::int16_t>(std::lround(value * 32767.f));
            }
        }

        mediaBuffer->Unlock();
        mediaBuffer->SetCurrentLength(sizeInBytes);

        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample)))
            return;

        sample->AddBuffer(mediaBuffer.Get());
        sample->SetSampleTime(toMediaTime(ptsSeconds));
        sample->SetSampleDuration(
            toMediaTime((double) frames / audioSpec->sampleRate));

        auto lock = std::lock_guard {writeMutex};
        writer->WriteSample(audioStreamIndex, sample.Get());
    }

    Threads::Async<void> finish() override
    {
        auto promise = Threads::AsyncPromise<void> {};
        auto result = promise.get();

        {
            auto lock = std::lock_guard {writeMutex};

            if (writer)
            {
                writer->Finalize();
                writer.Reset();
            }
        }

        // Finalize is synchronous, so the file is fully written by here.
        promise.resolve();
        return result;
    }

    ComPtr<IMFSinkWriter> writer;
    DWORD streamIndex = 0;
    DWORD audioStreamIndex = 0;
    std::optional<AudioSpec> audioSpec;

    // Frames arrive on the capture thread and samples on the recorder's drain;
    // the sink writer takes one call at a time.
    std::mutex writeMutex;

    int width = 0;
    int height = 0;
    int fps = 60;
    bool comInitialized = false;
    bool mfStarted = false;
};
} // namespace

OwningPointer<Encoder> makeEncoder()
{
    return makeOwned<WindowsEncoder>();
}

} // namespace eacp::Video
