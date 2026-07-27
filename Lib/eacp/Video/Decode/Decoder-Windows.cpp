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

// Windows decode backend (Media Foundation IMFSourceReader). Frames are taken
// in NV12 — what the H.264/HEVC decoders produce natively — so no colour
// conversion happens on the way out; VideoView uploads the two planes and the
// shader converts. That is 1.5 bytes per pixel through the copy and the upload
// instead of 4.
//
// The reader is bound to a D3D11 device where one with a decoder exists, which
// is what moves the decode itself onto the GPU's video engine; where none does
// — a VM, a remote session, a frozen driver — the same code decodes in software
// and nothing else changes.
//
// Frames still arrive as CPU buffers and are copied out tightly packed, the
// same upload path Cameras::Camera takes on this platform (Texture::update).
// Zero-copy needs those NV12 textures shared into the D3D12 device instead of
// read back, which is the same later phase the camera backend is waiting on.

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

// COM is per-thread state: initialised on every thread that calls into Media
// Foundation and released on that same thread. This decoder is driven from two
// — open() by whoever opened the stream, nextFrame() and seek() by FrameStream's
// decode thread — so a single flag on the decoder cannot describe it, and a
// CoUninitialize() from a destructor is not necessarily even on the thread that
// initialised. A thread_local gives the apartment the one lifetime COM accepts.
//
// The source reader in practice tolerates a decode thread with no apartment at
// all, hardware decode included; this is the contract rather than a workaround.
void joinComApartment()
{
    struct Apartment
    {
        // Multithreaded is what Media Foundation wants, and the decode thread
        // has no apartment to lose. The event loop thread is already STA (see
        // EventLoop-Windows: WebView2, the shell dialogs and DirectComposition
        // need it there), so this reports RPC_E_CHANGED_MODE and leaves it
        // alone — nothing was joined, so there is nothing to leave.
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

// A D3D11 device Media Foundation can decode on, or null. Null is not an error:
// it is the software path this backend has always taken, and it is where a VM,
// a remote desktop session or an OEM-frozen driver lands.
//
// The decoder-profile count is the capability check, and it is not the same
// question as whether the adapter does video at all. An adapter can expose a
// video *processor* — fixed-function colour conversion, scaling, deinterlace —
// with no decoder behind it, and binding a manager to that accelerates nothing
// while putting a device in the pipeline.
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

// Copies both planes of a locked NV12 buffer into `out`, tightly packed. The
// planes are contiguous in the source too — `height` luma rows followed by
// `height / 2` chroma rows, all at the same stride — so this is two runs of the
// same row copy, and the destination keeps the layout VideoFrame documents.
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
        // three go before the platform they were created on is shut down.
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

        // Only a safety net now that NV12 is what is asked for: the decoders
        // produce it natively, so no processor is inserted in the common case.
        // It stays for the odd source that decodes to something else, which
        // would otherwise fail to open rather than cost a conversion.
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

        // NV12 is what the H.264/HEVC decoders produce natively, so asking for
        // it means no video processor is inserted at all — the frames come
        // straight out of the decoder. Asking for RGB32 instead costs a
        // full-frame colour conversion on the CPU for every frame, which at 8K
        // is the single most expensive thing in the decode path.
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

        // A read can legitimately return no sample (a stream tick, a format
        // change) without being the end of the file, so keep asking until one
        // arrives. The bound stops a malformed file spinning here forever.
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

            // The video processor can be re-negotiated mid-file; the frame size
            // may have changed with it.
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

            // Media Foundation seeks to the keyframe at or before the target, so
            // an accurate seek is finished here: drop everything that ends
            // before the requested time.
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

        // Keyframe mode keeps whatever the seek landed on; Accurate decodes
        // forward from there and throws away the frames before the target.
        discardBefore = moved && mode == SeekMode::Accurate ? target : 0.0;
    }

private:
    // Points the reader at a D3D11 device so Media Foundation puts the decode
    // on the GPU's video engine. Everything downstream is unaffected: the
    // frames still come back through Lock2D as CPU pixels, they are just
    // produced by fixed-function silicon instead of the CPU.
    //
    // Failure here is not propagated. Every step of it is a capability
    // question, and the answer "no" means the software decoder, which is what
    // this file did before any of this existed.
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

        // Tracks below high definition are usually BT.601 and above it BT.709,
        // but only the file can say for sure, so the height rule is the
        // fallback rather than the answer.
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
        // pitch itself. One luma sample per byte, so an unpadded NV12 row is
        // just the width.
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

    // A buffer no live frame is still reading, or a new one. Recycling these
    // rather than allocating per frame saves an allocation, a page fault per
    // page and a full zero-fill of the buffer on every frame: resize() on a
    // buffer that is already the right size does nothing, where a fresh Vector
    // value-initialises all of it before the decoder overwrites every byte. At
    // 8K that is 133 MB of pointless writes per frame.
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

        // Lock2D reports the real stride. Not every buffer implements it, hence
        // the plain Lock fallback onto the media type's default stride. Unlike
        // RGB there is no bottom-up case to handle: NV12 is always top-down.
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

            // Both planes have to be there: luma rows plus half as many chroma
            // rows, all at the same stride.
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

    // The track's colour coding, read from the media type when it says and
    // inferred from the frame height when it does not.
    YuvMatrix yuvMatrix = YuvMatrix::BT709;
    bool fullRangeYuv = false;

    // Grows to the number of frames alive at once — the stream's queue depth
    // plus the one on screen — and then stops.
    Vector<std::shared_ptr<Vector<std::uint8_t>>> pixelBuffers;

    // Set by an accurate seek: frames ending before this are decoded and
    // dropped so the first frame handed out is the one covering the target.
    double discardBefore = 0.0;

    bool mfStarted = false;
};

OwningPointer<Decoder> makeDecoder()
{
    return makeOwned<WindowsDecoder>();
}

// Media Foundation frames always arrive as CPU pixels here, so toImage never
// reaches this. It grows a body when the reader is bound to a D3D11 device and
// starts handing back textures instead.
Graphics::Image nativeBufferToImage(void*)
{
    return {};
}
} // namespace eacp::Video
