#include "Image.h"

#include "ImageCodec.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace eacp::Graphics
{

namespace
{
std::uint8_t toByte(float value)
{
    auto scaled = std::lround(std::clamp(value, 0.f, 1.f) * 255.f);
    return static_cast<std::uint8_t>(scaled);
}

std::size_t pixelCount(int width, int height)
{
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
}

std::size_t validatedPixelCount(int width, int height)
{
    if (width < 0 || height < 0)
        throw std::invalid_argument("Image: width and height must be non-negative");
    return pixelCount(width, height);
}

std::string lowerExtension(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    std::transform(ext.begin(),
                   ext.end(),
                   ext.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return ext;
}

ImageFormat formatFromExtension(const std::filesystem::path& path)
{
    auto ext = lowerExtension(path);
    if (ext == ".png")
        return ImageFormat::png;
    if (ext == ".jpg" || ext == ".jpeg")
        return ImageFormat::jpeg;

    throw std::runtime_error("Image::save: cannot infer format from extension '"
                             + ext + "' (supported: .png, .jpg, .jpeg)");
}

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path& path,
                                        std::string& error)
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

    auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (file.bad() || file.gcount() != static_cast<std::streamsize>(bytes.size()))
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
    , rgba(validatedPixelCount(widthToUse, heightToUse), 0)
{
}

Image::Image(int widthToUse, int heightToUse, std::vector<std::uint8_t> pixelsToUse)
    : w(widthToUse)
    , h(heightToUse)
    , rgba(std::move(pixelsToUse))
{
    if (rgba.size() != validatedPixelCount(w, h))
        throw std::invalid_argument("Image: pixel buffer size does not match "
                                    "width * height * 4");
}

std::optional<Image>
    Image::decode(const std::uint8_t* data, std::size_t size, std::string* error)
{
    auto err = std::string {};
    auto decoded = detail::decodeImageBytes(data, size, err);
    if (!decoded)
    {
        if (error != nullptr)
            *error = err;
        return std::nullopt;
    }

    return Image(decoded->width, decoded->height, std::move(decoded->rgba));
}

std::optional<Image> Image::decode(const std::vector<std::uint8_t>& bytes,
                                   std::string* error)
{
    return decode(bytes.data(), bytes.size(), error);
}

std::optional<Image> Image::load(const std::filesystem::path& path,
                                 std::string* error)
{
    auto err = std::string {};
    auto bytes = readFileBytes(path, err);
    if (!err.empty())
    {
        if (error != nullptr)
            *error = err;
        return std::nullopt;
    }

    return decode(bytes, error);
}

bool Image::isValid() const
{
    return w > 0 && h > 0 && rgba.size() == pixelCount(w, h);
}

bool Image::isEmpty() const
{
    return rgba.empty();
}

int Image::width() const
{
    return w;
}

int Image::height() const
{
    return h;
}

const std::vector<std::uint8_t>& Image::pixels() const
{
    return rgba;
}

Color Image::at(int x, int y) const
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return {0.f, 0.f, 0.f, 0.f};

    auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w)
              + static_cast<std::size_t>(x))
             * 4;
    return {static_cast<float>(rgba[i]) / 255.f,
            static_cast<float>(rgba[i + 1]) / 255.f,
            static_cast<float>(rgba[i + 2]) / 255.f,
            static_cast<float>(rgba[i + 3]) / 255.f};
}

void Image::set(int x, int y, const Color& color)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;

    auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w)
              + static_cast<std::size_t>(x))
             * 4;
    rgba[i] = toByte(color.r);
    rgba[i + 1] = toByte(color.g);
    rgba[i + 2] = toByte(color.b);
    rgba[i + 3] = toByte(color.a);
}

std::vector<std::uint8_t> Image::encode(ImageFormat format, float quality) const
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

std::vector<std::uint8_t> Image::toPng() const
{
    return encode(ImageFormat::png);
}

std::vector<std::uint8_t> Image::toJpeg(float quality) const
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

    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
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
