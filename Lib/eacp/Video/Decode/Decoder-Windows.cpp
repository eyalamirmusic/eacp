#include <eacp/Core/Utils/WinInclude.h>

#include "Decoder.h"

#include "FrameImage.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <memory>

// Media Foundation IMFSourceReader. Frames are taken in NV12, what the
// H.264/HEVC decoders produce natively, and arrive as CPU buffers copied out
// tightly packed; the shader does the colour conversion. No zero-copy path yet.

namespace eacp::Video
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr auto hundredNanosPerSecond = 10'000'000.0;

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

// COM apartments are per-thread state, and this decoder is driven from two
// threads: open() by the caller, nextFrame()/seek() by the decode thread. The
// thread_local gives the apartment the one lifetime COM accepts.
void joinComApartment()
{
    struct Apartment
    {
        // The event loop thread is already STA, so this reports
        // RPC_E_CHANGED_MODE and leaves it alone; nothing was joined.
        Apartment()
            : joined {SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))}
        {
        }

        ~Apartment()
        {
            if (joined)
                CoUninitialize();
        }

        const bool joined;
    };

    thread_local auto apartment = Apartment {};
}

// Null means the software path, not an error. An adapter can expose a video
// *processor* with no decoder behind it, so the decoder-profile count rather
// than video support is the capability check.
ComPtr<ID3D11Device> createVideoDevice()
{
    ComPtr<ID3D11Device> device;

    if (FAILED(D3D11CreateDevice(nullptr,
                                 D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 nullptr,
                                 0,
                                 D3D11_SDK_VERSION,
                                 &device,
                                 nullptr,
                                 nullptr)))
        return nullptr;

    ComPtr<ID3D11VideoDevice> videoDevice;

    if (FAILED(device.As(&videoDevice))
        || videoDevice->GetVideoDecoderProfileCount() == 0)
        return nullptr;

    // Media Foundation drives the device from its own worker threads, alongside
    // the decode thread reading frames back off it.
    ComPtr<ID3D10Multithread> multithread;

    if (FAILED(device.As(&multithread)))
        return nullptr;

    multithread->SetMultithreadProtected(TRUE);
    return device;
}

int rotationFromAttribute(UINT32 rotation)
{
    switch (rotation)
    {
        case MFVideoRotationFormat_90:
            return 90;
        case MFVideoRotationFormat_180:
            return 180;
        case MFVideoRotationFormat_270:
            return 270;
        default:
            return 0;
    }
}

// Both planes of a locked NV12 buffer into `out`, tightly packed. They are
// contiguous in the source too: `height` luma rows then `height / 2` chroma
// rows, all at the same stride.
void copyPlanes(const std::uint8_t* source,
                LONG stride,
                int width,
                int height,
                Vector<std::uint8_t>& out)
{
    auto rowBytes = static_cast<std::size_t>(width);
    auto chromaRows = height / 2;

    out.resize((int) framePixelBytes(FramePixelFormat::NV12, width, height));

    auto copyRows = [&](std::ptrdiff_t sourceRow, std::size_t destRow, int rows)
    {
        for (auto y = 0; y < rows; ++y)
        {
            const auto* from =
                source + (sourceRow + y) * static_cast<std::ptrdiff_t>(stride);
            std::memcpy(
                out.data() + (destRow + (std::size_t) y) * rowBytes, from, rowBytes);
        }
    };

    copyRows(0, 0, height);
    copyRows(height, (std::size_t) height, chromaRows);
}
} // namespace

struct WindowsDecoder final : Decoder
{
    ~WindowsDecoder() override
    {
        // The reader holds the device manager, which holds the device; all
        // three go before MFShutdown.
        reader.Reset();
        deviceManager.Reset();
        d3dDevice.Reset();

        if (mfStarted)
            MFShutdown();
    }

    bool open(const FilePath& path) override
    {
        joinComApartment();
        mfStarted = SUCCEEDED(MFStartup(MF_VERSION));

        if (!mfStarted)
            return false;

        ComPtr<IMFAttributes> attributes;
        if (FAILED(MFCreateAttributes(&attributes, 2)))
            return false;

        bindVideoDevice(*attributes.Get());

        // A safety net for the odd source that decodes to something other than
        // NV12, which would otherwise fail to open.
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
                              TRUE);

        auto url = widen(path.c_str());
        if (FAILED(
                MFCreateSourceReaderFromURL(url.c_str(), attributes.Get(), &reader)))
            return false;

        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

        ComPtr<IMFMediaType> outputType;
        if (FAILED(MFCreateMediaType(&outputType)))
            return false;

        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

        // Asking for the decoders' native format keeps a video processor, and
        // its per-frame CPU colour conversion, out of the pipeline.
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);

        if (FAILED(reader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get())))
            return false;

        return readTrackInfo();
    }

    VideoInfo info() const override { return videoInfo; }

    bool nextFrame(VideoFrame& out) override
    {
        if (reader == nullptr)
            return false;

        joinComApartment();

        // A read can return no sample (a stream tick, a format change) without
        // being the end of the file; the bound stops a malformed file spinning.
        constexpr auto maxEmptyReads = 128;

        for (auto attempt = 0; attempt < maxEmptyReads; ++attempt)
        {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;

            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                          0,
                                          nullptr,
                                          &flags,
                                          &timestamp,
                                          &sample)))
                return false;

            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
                return false;

            // Re-negotiated mid-file, so the frame size may have changed.
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
                readTrackInfo();

            if (sample == nullptr)
                continue;

            LONGLONG sampleDuration = 0;
            if (FAILED(sample->GetSampleDuration(&sampleDuration)))
                sampleDuration = 0;

            auto seconds = static_cast<double>(timestamp) / hundredNanosPerSecond;
            auto duration =
                static_cast<double>(sampleDuration) / hundredNanosPerSecond;

            if (duration <= 0.0 && videoInfo.frameRate > 0.0)
                duration = 1.0 / videoInfo.frameRate;

            // Media Foundation seeks to the keyframe at or before the target,
            // so an accurate seek finishes here.
            if (seconds + duration <= discardBefore)
                continue;

            discardBefore = 0.0;

            if (!buildFrame(sample.Get(), seconds, duration, out))
                continue;

            return true;
        }

        return false;
    }

    void seek(double seconds, SeekMode mode) override
    {
        if (reader == nullptr)
            return;

        joinComApartment();

        auto target = std::max(0.0, seconds);

        PROPVARIANT position;
        InitPropVariantFromInt64(
            static_cast<LONGLONG>(target * hundredNanosPerSecond), &position);

        auto moved = SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, position));
        PropVariantClear(&position);

        discardBefore = moved && mode == SeekMode::Accurate ? target : 0.0;
    }

private:
    // Moves the decode onto the GPU's video engine; frames still come back
    // through Lock2D as CPU pixels. Failure is not propagated — every step is a
    // capability question whose "no" means the software decoder.
    void bindVideoDevice(IMFAttributes& attributes)
    {
        d3dDevice = createVideoDevice();

        if (d3dDevice == nullptr)
            return;

        UINT resetToken = 0;

        if (FAILED(MFCreateDXGIDeviceManager(&resetToken, &deviceManager))
            || FAILED(deviceManager->ResetDevice(d3dDevice.Get(), resetToken)))
        {
            deviceManager.Reset();
            d3dDevice.Reset();
            return;
        }

        attributes.SetUnknown(MF_SOURCE_READER_D3D_MANAGER, deviceManager.Get());
    }

    bool readTrackInfo()
    {
        ComPtr<IMFMediaType> currentType;
        if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               &currentType)))
            return false;

        UINT32 width = 0;
        UINT32 height = 0;
        if (FAILED(MFGetAttributeSize(
                currentType.Get(), MF_MT_FRAME_SIZE, &width, &height)))
            return false;

        videoInfo.width = static_cast<int>(width);
        videoInfo.height = static_cast<int>(height);

        UINT32 rateNumerator = 0;
        UINT32 rateDenominator = 0;
        if (SUCCEEDED(MFGetAttributeRatio(currentType.Get(),
                                          MF_MT_FRAME_RATE,
                                          &rateNumerator,
                                          &rateDenominator))
            && rateDenominator != 0)
            videoInfo.frameRate =
                static_cast<double>(rateNumerator) / rateDenominator;

        UINT32 rotation = MFVideoRotationFormat_0;
        if (SUCCEEDED(currentType->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation)))
            videoInfo.rotationDegrees = rotationFromAttribute(rotation);

        // The height rule is the fallback; only the file can say for sure.
        yuvMatrix = yuvMatrixForHeight(videoInfo.height);

        UINT32 signalledMatrix = 0;
        if (SUCCEEDED(currentType->GetUINT32(MF_MT_YUV_MATRIX, &signalledMatrix)))
            yuvMatrix = signalledMatrix == MFVideoTransferMatrix_BT601
                            ? YuvMatrix::BT601
                            : YuvMatrix::BT709;

        UINT32 nominalRange = 0;
        if (SUCCEEDED(
                currentType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominalRange)))
            fullRangeYuv = nominalRange == MFNominalRange_0_255;

        // Only consulted by the plain-Lock fallback; Lock2D reports the real
        // pitch itself. An unpadded NV12 row is one byte per luma sample.
        LONG defaultStride = 0;
        if (SUCCEEDED(currentType->GetUINT32(
                MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&defaultStride))))
            stride = defaultStride;
        else
            stride = static_cast<LONG>(width);

        PROPVARIANT durationValue;
        PropVariantInit(&durationValue);

        if (SUCCEEDED(reader->GetPresentationAttribute(
                MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &durationValue)))
        {
            LONGLONG hundredNanos = 0;
            if (SUCCEEDED(PropVariantToInt64(durationValue, &hundredNanos)))
                videoInfo.duration =
                    static_cast<double>(hundredNanos) / hundredNanosPerSecond;
        }

        PropVariantClear(&durationValue);
        return videoInfo.width > 0 && videoInfo.height > 0;
    }

    // A buffer no live frame is still reading, or a new one. Recycling avoids
    // an allocation and a full zero-fill on every frame.
    std::shared_ptr<Vector<std::uint8_t>> acquirePixelBuffer()
    {
        for (auto& candidate: pixelBuffers)
            if (candidate.use_count() == 1)
                return candidate;

        auto fresh = std::make_shared<Vector<std::uint8_t>>();
        pixelBuffers.add(fresh);
        return fresh;
    }

    bool buildFrame(IMFSample* sample,
                    double seconds,
                    double duration,
                    VideoFrame& out)
    {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            return false;

        auto pixels = acquirePixelBuffer();
        auto copied = false;

        // Lock2D reports the real stride but is not always implemented, hence
        // the plain Lock fallback. NV12 is always top-down; row 0 is the top.
        ComPtr<IMF2DBuffer> buffer2D;

        if (SUCCEEDED(buffer.As(&buffer2D)))
        {
            BYTE* scanline0 = nullptr;
            LONG pitch = 0;

            if (SUCCEEDED(buffer2D->Lock2D(&scanline0, &pitch)) && pitch > 0)
            {
                copyPlanes(
                    scanline0, pitch, videoInfo.width, videoInfo.height, *pixels);
                buffer2D->Unlock2D();
                copied = true;
            }
        }

        if (!copied)
        {
            BYTE* data = nullptr;
            DWORD maxLength = 0;
            DWORD currentLength = 0;

            if (FAILED(buffer->Lock(&data, &maxLength, &currentLength)))
                return false;

            auto planeBytes = static_cast<DWORD>(stride * videoInfo.height);
            auto enough = stride > 0 && currentLength >= planeBytes + planeBytes / 2;

            if (enough)
                copyPlanes(data, stride, videoInfo.width, videoInfo.height, *pixels);

            buffer->Unlock();

            if (!enough)
                return false;
        }

        auto frameInfo = FrameInfo {};
        frameInfo.width = videoInfo.width;
        frameInfo.height = videoInfo.height;
        frameInfo.format = FramePixelFormat::NV12;
        frameInfo.bytesPerRow = static_cast<std::size_t>(videoInfo.width);
        frameInfo.yuvMatrix = yuvMatrix;
        frameInfo.fullRangeYuv = fullRangeYuv;
        frameInfo.seconds = seconds;
        frameInfo.duration = duration;

        out = VideoFrame::fromPixelBuffer(std::move(pixels), frameInfo);
        return true;
    }

    ComPtr<IMFSourceReader> reader;

    // Null on anything with no hardware decoder, which is the software path.
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<IMFDXGIDeviceManager> deviceManager;

    VideoInfo videoInfo;
    LONG stride = 0;

    YuvMatrix yuvMatrix = YuvMatrix::BT709;
    bool fullRangeYuv = false;

    // Grows to the number of frames alive at once, then stops.
    Vector<std::shared_ptr<Vector<std::uint8_t>>> pixelBuffers;

    // Set by an accurate seek: frames ending before this are decoded and
    // dropped.
    double discardBefore = 0.0;

    bool mfStarted = false;
};

OwningPointer<Decoder> makeDecoder()
{
    return makeOwned<WindowsDecoder>();
}

// Frames always arrive as CPU pixels here, so toImage never reaches this.
Graphics::Image nativeBufferToImage(void*)
{
    return {};
}
} // namespace eacp::Video
