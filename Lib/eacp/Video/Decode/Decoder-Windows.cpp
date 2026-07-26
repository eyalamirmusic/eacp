#include <eacp/Core/Utils/WinInclude.h>

#include "Decoder.h"

#include "FrameImage.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

// Windows decode backend (Media Foundation IMFSourceReader). The reader's
// advanced video processing converts whatever the file holds into RGB32, which
// is BGRA in memory order and so matches GPU::TextureFormat::BGRA8Unorm.
//
// Frames arrive as CPU buffers and are copied out tightly packed, the same
// upload path Cameras::Camera takes on this platform (Texture::update).
// Zero-copy needs the reader bound to a D3D11 device and its NV12 textures
// shared into the D3D12 device, which is the same later phase the camera
// backend is waiting on.
//
// NOTE: authored to the documented IMFSourceReader pattern and this repo's
// Media Foundation conventions, but not yet compiled or run on Windows —
// expect a round of fixes on the first Windows/CI build.

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

// Copies one locked BGRA plane into `out`, tightly packed and top-down. A
// negative stride means Media Foundation handed back a bottom-up image, so the
// rows are read in reverse from the last one.
void copyRows(const std::uint8_t* source,
              LONG stride,
              int width,
              int height,
              Vector<std::uint8_t>& out)
{
    auto rowBytes = static_cast<std::size_t>(width) * 4;
    out.resize(height * width * 4);

    for (auto y = 0; y < height; ++y)
    {
        const auto* row = source + static_cast<std::ptrdiff_t>(y) * stride;
        std::memcpy(
            out.data() + static_cast<std::size_t>(y) * rowBytes, row, rowBytes);
    }
}
} // namespace

struct WindowsDecoder final : Decoder
{
    ~WindowsDecoder() override
    {
        reader.Reset();

        if (mfStarted)
            MFShutdown();

        if (comInitialized)
            CoUninitialize();
    }

    bool open(const FilePath& path) override
    {
        comInitialized =
            SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        mfStarted = SUCCEEDED(MFStartup(MF_VERSION));

        if (!mfStarted)
            return false;

        ComPtr<IMFAttributes> attributes;
        if (FAILED(MFCreateAttributes(&attributes, 1)))
            return false;

        // Lets the reader insert the video processor that gives us RGB32 out of
        // whatever the decoder natively produces (NV12, almost always).
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
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

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

        // A negative default stride is Media Foundation's way of saying the
        // image is bottom-up; buildFrame flips those rows on the way out.
        LONG defaultStride = 0;
        if (SUCCEEDED(currentType->GetUINT32(
                MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&defaultStride))))
            stride = defaultStride;
        else
            stride = static_cast<LONG>(width) * 4;

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

    bool buildFrame(IMFSample* sample,
                    double seconds,
                    double duration,
                    VideoFrame& out)
    {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            return false;

        auto pixels = Vector<std::uint8_t> {};
        auto copied = false;

        // Lock2D reports the real stride, including the sign that says whether
        // the image is bottom-up. Not every buffer implements it, hence the
        // plain Lock fallback onto the media type's default stride.
        ComPtr<IMF2DBuffer> buffer2D;

        if (SUCCEEDED(buffer.As(&buffer2D)))
        {
            BYTE* scanline0 = nullptr;
            LONG pitch = 0;

            if (SUCCEEDED(buffer2D->Lock2D(&scanline0, &pitch)))
            {
                copyRows(
                    scanline0, pitch, videoInfo.width, videoInfo.height, pixels);
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

            // A bottom-up default stride points the first row at the last
            // scanline, which is what copyRows walks backwards from.
            auto rowBytes = static_cast<std::size_t>(videoInfo.width) * 4;
            auto absoluteStride = std::abs(stride);
            const auto* first =
                stride < 0 ? data
                                 + static_cast<std::ptrdiff_t>(videoInfo.height - 1)
                                       * absoluteStride
                           : data;

            if (currentLength
                >= static_cast<DWORD>(absoluteStride * videoInfo.height))
                copyRows(first, stride, videoInfo.width, videoInfo.height, pixels);
            else
                rowBytes = 0;

            buffer->Unlock();

            if (rowBytes == 0)
                return false;
        }

        auto frameInfo = FrameInfo {};
        frameInfo.width = videoInfo.width;
        frameInfo.height = videoInfo.height;
        frameInfo.bytesPerRow = static_cast<std::size_t>(videoInfo.width) * 4;
        frameInfo.seconds = seconds;
        frameInfo.duration = duration;

        out = VideoFrame::fromPixels(std::move(pixels), frameInfo);
        return true;
    }

    ComPtr<IMFSourceReader> reader;
    VideoInfo videoInfo;
    LONG stride = 0;

    // Set by an accurate seek: frames ending before this are decoded and
    // dropped so the first frame handed out is the one covering the target.
    double discardBefore = 0.0;

    bool comInitialized = false;
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
