#include <eacp/Core/Utils/WinInclude.h>

#include "Encoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <string>

// The Windows encoder: a Media Foundation IMFSinkWriter writing H.264 into an
// .mp4. Each snapshot frame is composited over black into a BGRA (RGB32) sample
// and written with a real-time presentation timestamp; the SinkWriter's implicit
// converter feeds the H.264 encoder, so playback runs at capture speed. The
// GpuDirect tier is not wired here yet (no D3D->MF zero-copy), so those hooks
// keep the base's "unsupported" default and callers fall back to Snapshot.

namespace eacp::Video
{
namespace
{
using Microsoft::WRL::ComPtr;

// FilePath carries UTF-8; Media Foundation wants a wide URL.
std::wstring widen(const char* utf8)
{
    auto length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (length <= 0)
        return {};

    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), length);
    if (!wide.empty() && wide.back() == L'\0')
        wide.pop_back();

    return wide;
}

// One 100-nanosecond-tick duration for a frame at `fps`, the unit MF timestamps
// use.
LONGLONG frameDuration(int fps)
{
    return 10'000'000LL / (fps > 0 ? fps : 60);
}

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

    bool
        begin(const FilePath& path, int w, int h, int bitrate, int fpsToUse) override
    {
        width = w;
        height = h;
        fps = fpsToUse > 0 ? fpsToUse : 60;

        // MF needs COM up on this thread. The GUI thread is usually already
        // apartment-initialized; a matching re-init returns S_FALSE (still ours
        // to balance), only a conflicting mode fails -- then we do not un-init.
        auto comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        comInitialized = SUCCEEDED(comResult);

        if (FAILED(MFStartup(MF_VERSION)))
            return false;
        mfStarted = true;

        auto url = widen(path.c_str());
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

        if (!configureStreams(bitrate))
        {
            writer.Reset();
            return false;
        }

        return SUCCEEDED(writer->BeginWriting());
    }

    bool configureStreams(int bitrate)
    {
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

        return SUCCEEDED(
            writer->SetInputMediaType(streamIndex, inputType.Get(), nullptr));
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
        sample->SetSampleTime(static_cast<LONGLONG>(std::llround(ptsSeconds * 1e7)));
        sample->SetSampleDuration(frameDuration(fps));

        writer->WriteSample(streamIndex, sample.Get());
    }

    Threads::Async<void> finish() override
    {
        auto promise = Threads::AsyncPromise<void> {};
        auto result = promise.get();

        if (writer)
        {
            writer->Finalize();
            writer.Reset();
        }

        // Finalize is synchronous, so the file is fully written by here.
        promise.resolve();
        return result;
    }

    ComPtr<IMFSinkWriter> writer;
    DWORD streamIndex = 0;
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
