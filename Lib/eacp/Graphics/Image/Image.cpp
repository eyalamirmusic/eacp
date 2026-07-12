#include "Image.h"

#include "ImageCodec.h"

#include <fstream>
#include <ios>

namespace eacp::Graphics
{

namespace
{
std::uint8_t toByte(float value)
{
    auto scaled = std::lround(std::clamp(value, 0.f, 1.f) * 255.f);
    return static_cast<std::uint8_t>(scaled);
}

// Byte count for the given dimensions, or -1 when they are negative or
// the result would overflow an int (ImageData is int-sized).
int byteCountFor(int width, int height)
{
    if (width < 0 || height < 0)
        return -1;

    constexpr auto maxPixels = std::numeric_limits<int>::max() / 4;
    if (width != 0 && height > maxPixels / width)
        return -1;

    return width * height * 4;
}

int validatedByteCount(int width, int height)
{
    auto bytes = byteCountFor(width, height);
    if (bytes < 0)
        throw std::invalid_argument(
            "Image: dimensions are negative or exceed the maximum buffer size");

    return bytes;
}

ImageFormat formatFromExtension(const std::filesystem::path& path)
{
    auto ext = Strings::toLower(path.extension().string());
    if (ext == ".png")
        return ImageFormat::png;
    if (ext == ".jpg" || ext == ".jpeg")
        return ImageFormat::jpeg;

    throw std::runtime_error("Image::save: cannot infer format from extension '"
                             + ext + "' (supported: .png, .jpg, .jpeg)");
}

ImageData readFileBytes(const std::filesystem::path& path, std::string& error)
{
    auto file = std::ifstream {path, std::ios::binary | std::ios::ate};
    if (!file)
    {
        error = "cannot open '" + path.string() + "'";
        return {};
    }

    auto size = file.tellg();
    if (size < 0)
    {
        error = "cannot determine size of '" + path.string() + "'";
        return {};
    }

    auto offset = static_cast<std::streamoff>(size);
    if (offset > static_cast<std::streamoff>(std::numeric_limits<int>::max()))
    {
        error = "'" + path.string() + "' is too large to load";
        return {};
    }

    auto bytes = ImageData(static_cast<int>(offset));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (file.bad() || file.gcount() != bytes.size())
    {
        error = "failed to read '" + path.string() + "'";
        return {};
    }

    return bytes;
}
} // namespace

Image::Image(int widthToUse, int heightToUse)
    : w(widthToUse)
    , h(heightToUse)
    , rgba(validatedByteCount(widthToUse, heightToUse))
{
}

Image::Image(int widthToUse, int heightToUse, ImageData pixelsToUse)
    : w(widthToUse)
    , h(heightToUse)
    , rgba(std::move(pixelsToUse))
{
    if (rgba.size() != validatedByteCount(w, h))
        throw std::invalid_argument("Image: pixel buffer size does not match "
                                    "width * height * 4");
}

Image Image::decode(const std::uint8_t* data, int size, std::string* error)
{
    auto err = std::string {};
    auto image = detail::decodeImageBytes(data, size, err);
    if (!image && error != nullptr)
        *error = err;

    return image;
}

Image Image::decode(const ImageData& bytes, std::string* error)
{
    return decode(bytes.data(), bytes.size(), error);
}

Image Image::load(const std::filesystem::path& path, std::string* error)
{
    auto err = std::string {};
    auto bytes = readFileBytes(path, err);
    if (!err.empty())
    {
        if (error != nullptr)
            *error = err;
        return {};
    }

    return decode(bytes, error);
}

bool Image::isValid() const
{
    auto bytes = byteCountFor(w, h);
    return bytes > 0 && rgba.size() == bytes;
}

bool Image::isEmpty() const
{
    return rgba.empty();
}

Image::operator bool() const
{
    return isValid();
}

int Image::width() const
{
    return w;
}

int Image::height() const
{
    return h;
}

const ImageData& Image::pixels() const
{
    return rgba;
}

Color Image::at(int x, int y) const
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return {0.f, 0.f, 0.f, 0.f};

    auto i = (y * w + x) * 4;
    const auto* p = rgba.data();
    return {static_cast<float>(p[i]) / 255.f,
            static_cast<float>(p[i + 1]) / 255.f,
            static_cast<float>(p[i + 2]) / 255.f,
            static_cast<float>(p[i + 3]) / 255.f};
}

void Image::set(int x, int y, const Color& color)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;

    auto i = (y * w + x) * 4;
    auto* p = rgba.data();
    p[i] = toByte(color.r);
    p[i + 1] = toByte(color.g);
    p[i + 2] = toByte(color.b);
    p[i + 3] = toByte(color.a);
}

std::uint8_t* Image::prepareForOverwrite(int width, int height)
{
    auto bytes = byteCountFor(width, height);
    if (bytes <= 0) // non-positive dimension or would overflow int
    {
        w = 0;
        h = 0;
        rgba.clear();
        return nullptr;
    }

    w = width;
    h = height;
    // Resize only on a size change: a same-size call is a no-op, so a recycled
    // image skips both the reallocation and std::vector's zero-fill of the new
    // bytes -- which the writer would immediately overwrite anyway.
    if (rgba.size() != bytes)
        rgba.resize(bytes);
    return rgba.data();
}

ImageData Image::encode(ImageFormat format, float quality) const
{
    if (!isValid())
        throw std::runtime_error("Image::encode: image is empty or invalid");

    auto error = std::string {};
    auto bytes = detail::encodeImageBytes(rgba.data(), w, h, format, quality, error);
    if (!error.empty() || bytes.empty())
        throw std::runtime_error("Image::encode: "
                                 + (error.empty() ? "encoding failed" : error));

    return bytes;
}

ImageData Image::toPng() const
{
    return encode(ImageFormat::png);
}

ImageData Image::toJpeg(float quality) const
{
    return encode(ImageFormat::jpeg, quality);
}

void Image::save(const std::filesystem::path& path) const
{
    save(path, formatFromExtension(path));
}

void Image::save(const std::filesystem::path& path,
                 ImageFormat format,
                 float quality) const
{
    auto bytes = encode(format, quality);

    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());

    auto file = std::ofstream {path, std::ios::binary | std::ios::trunc};
    if (!file)
        throw std::runtime_error("Image::save: failed to open '" + path.string()
                                 + "' for writing");

    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!file)
        throw std::runtime_error("Image::save: short write to '" + path.string()
                                 + "'");
}

bool Image::equals(const Image& other) const
{
    return w == other.w && h == other.h && rgba == other.rgba;
}

bool Image::operator==(const Image& other) const
{
    return equals(other);
}

bool Image::operator!=(const Image& other) const
{
    return !equals(other);
}

} // namespace eacp::Graphics
