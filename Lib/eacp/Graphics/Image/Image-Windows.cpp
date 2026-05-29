#include "ImageCodec.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace eacp::Graphics::detail
{
namespace
{
using Microsoft::WRL::ComPtr;

// WIC needs an initialized COM apartment. Tolerate a thread that was
// already initialized with a different concurrency model, and only
// balance the call that actually initialized.
class ComScope
{
public:
    ComScope()
        : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }

    ~ComScope()
    {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr;
};

ComPtr<IWICImagingFactory> createFactory()
{
    auto factory = ComPtr<IWICImagingFactory> {};
    CoCreateInstance(CLSID_WICImagingFactory,
                     nullptr,
                     CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&factory));
    return factory;
}

GUID containerFormat(ImageFormat format)
{
    return format == ImageFormat::png ? GUID_ContainerFormatPng
                                      : GUID_ContainerFormatJpeg;
}

// JPEG has no alpha channel; let the encoder negotiate down to BGR.
WICPixelFormatGUID framePixelFormat(ImageFormat format)
{
    return format == ImageFormat::png ? GUID_WICPixelFormat32bppRGBA
                                      : GUID_WICPixelFormat24bppBGR;
}
} // namespace

std::optional<DecodedImage>
    decodeImageBytes(const std::uint8_t* data, std::size_t size, std::string& error)
{
    if (data == nullptr || size == 0)
    {
        error = "empty image data";
        return std::nullopt;
    }

    auto com = ComScope {};
    if (!com.usable())
    {
        error = "COM initialization failed";
        return std::nullopt;
    }

    auto factory = createFactory();
    if (!factory)
    {
        error = "could not create WIC factory";
        return std::nullopt;
    }

    auto stream = ComPtr<IWICStream> {};
    if (FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                               static_cast<DWORD>(size))))
    {
        error = "could not wrap image bytes";
        return std::nullopt;
    }

    auto decoder = ComPtr<IWICBitmapDecoder> {};
    if (FAILED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
    {
        error = "unrecognized image format";
        return std::nullopt;
    }

    auto frame = ComPtr<IWICBitmapFrameDecode> {};
    if (FAILED(decoder->GetFrame(0, &frame)))
    {
        error = "could not read image frame";
        return std::nullopt;
    }

    auto converter = ComPtr<IWICFormatConverter> {};
    if (FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(frame.Get(),
                                        GUID_WICPixelFormat32bppRGBA,
                                        WICBitmapDitherTypeNone,
                                        nullptr,
                                        0.0,
                                        WICBitmapPaletteTypeCustom)))
    {
        error = "could not convert image to RGBA";
        return std::nullopt;
    }

    auto width = UINT {0};
    auto height = UINT {0};
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
    {
        error = "decoded image has zero dimensions";
        return std::nullopt;
    }

    auto decoded = DecodedImage {};
    decoded.width = static_cast<int>(width);
    decoded.height = static_cast<int>(height);
    decoded.rgba.assign(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);

    auto stride = width * 4;
    if (FAILED(converter->CopyPixels(nullptr,
                                     stride,
                                     static_cast<UINT>(decoded.rgba.size()),
                                     decoded.rgba.data())))
    {
        error = "could not read decoded pixels";
        return std::nullopt;
    }

    return decoded;
}

std::vector<std::uint8_t> encodeImageBytes(const std::uint8_t* rgba,
                                           int width,
                                           int height,
                                           ImageFormat format,
                                           float quality,
                                           std::string& error)
{
    auto com = ComScope {};
    if (!com.usable())
    {
        error = "COM initialization failed";
        return {};
    }

    auto factory = createFactory();
    if (!factory)
    {
        error = "could not create WIC factory";
        return {};
    }

    auto byteCount = static_cast<UINT>(static_cast<std::size_t>(width)
                                       * static_cast<std::size_t>(height) * 4);
    auto source = ComPtr<IWICBitmap> {};
    if (FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(width),
                                               static_cast<UINT>(height),
                                               GUID_WICPixelFormat32bppRGBA,
                                               static_cast<UINT>(width) * 4,
                                               byteCount,
                                               const_cast<BYTE*>(rgba),
                                               &source)))
    {
        error = "could not wrap pixel buffer";
        return {};
    }

    auto stream = ComPtr<IStream> {};
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
    {
        error = "could not allocate output stream";
        return {};
    }

    auto encoder = ComPtr<IWICBitmapEncoder> {};
    if (FAILED(factory->CreateEncoder(containerFormat(format), nullptr, &encoder))
        || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
    {
        error = "could not create image encoder";
        return {};
    }

    auto frame = ComPtr<IWICBitmapFrameEncode> {};
    auto options = ComPtr<IPropertyBag2> {};
    if (FAILED(encoder->CreateNewFrame(&frame, &options)))
    {
        error = "could not create encoder frame";
        return {};
    }

    if (format == ImageFormat::jpeg)
    {
        auto property = PROPBAG2 {};
        property.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        auto value = VARIANT {};
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = std::clamp(quality, 0.f, 1.f);
        options->Write(1, &property, &value);
    }

    auto framePixels = framePixelFormat(format);
    if (FAILED(frame->Initialize(options.Get()))
        || FAILED(
            frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))
        || FAILED(frame->SetPixelFormat(&framePixels)))
    {
        error = "could not configure encoder frame";
        return {};
    }

    if (FAILED(frame->WriteSource(source.Get(), nullptr)) || FAILED(frame->Commit())
        || FAILED(encoder->Commit()))
    {
        error = "image encoding failed";
        return {};
    }

    // The HGLOBAL behind the stream is rounded up past the bytes actually
    // written, so read the stream's logical length rather than GlobalSize.
    auto info = STATSTG {};
    if (FAILED(stream->Stat(&info, STATFLAG_NONAME)))
    {
        error = "could not read encoded bytes";
        return {};
    }

    auto length = static_cast<std::size_t>(info.cbSize.QuadPart);
    auto bytes = std::vector<std::uint8_t>(length);

    auto origin = LARGE_INTEGER {};
    auto read = ULONG {0};
    if (FAILED(stream->Seek(origin, STREAM_SEEK_SET, nullptr))
        || FAILED(stream->Read(bytes.data(), static_cast<ULONG>(length), &read))
        || read != length)
    {
        error = "could not read encoded bytes";
        return {};
    }

    return bytes;
}

} // namespace eacp::Graphics::detail
