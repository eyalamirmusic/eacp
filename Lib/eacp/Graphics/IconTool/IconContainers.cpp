#include "IconContainers.h"

#include "../Image/ImageOps.h"

namespace eacp::Graphics::Icons
{

namespace
{

using Bytes = Vector<std::uint8_t>;

void appendBE32(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void appendLE16(Bytes& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void appendLE32(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void appendTag(Bytes& out, const char* tag)
{
    for (auto i = 0; i < 4; ++i)
        out.add(static_cast<std::uint8_t>(tag[i]));
}

int maxDimension(const Image& src)
{
    return src.width() > src.height() ? src.width() : src.height();
}

Bytes pngFrame(const Image& src, int size)
{
    return downscaleTo(src, size).toPng();
}

// A standard size belongs in the container when the source is at least that
// big, so we never upscale — except the smallest size, always kept so even a
// tiny source yields a usable icon.
bool keepSize(int size, int limit, int smallest)
{
    return size <= limit || size == smallest;
}

} // namespace

Image downscaleTo(const Image& src, int size)
{
    auto current = src;
    while (current.width() / 2 >= size && current.height() / 2 >= size)
        current = resizeBilinear(current, current.width() / 2, current.height() / 2);

    return resizeBilinear(current, size, size);
}

void writeIcns(const Image& src, const FilePath& out)
{
    struct Chunk
    {
        int size;
        const char* type;
    };

    static constexpr Array chunks = {
        Chunk {1024, "ic10"},
        Chunk {512, "ic09"},
        Chunk {256, "ic08"},
        Chunk {128, "ic07"},
        Chunk {64, "icp6"},
        Chunk {32, "icp5"},
        Chunk {16, "icp4"},
    };

    const auto limit = maxDimension(src);

    Bytes body;
    for (const auto& chunk: chunks)
    {
        if (!keepSize(chunk.size, limit, 16))
            continue;

        const auto png = pngFrame(src, chunk.size);
        appendTag(body, chunk.type);
        appendBE32(body, static_cast<std::uint32_t>(8 + png.size()));
        body.addFrom(png);
    }

    Bytes file;
    appendTag(file, "icns");
    appendBE32(file, static_cast<std::uint32_t>(8 + body.size()));
    file.addFrom(body);

    Files::writeFile(out, file);
}

void writeIco(const Image& src, const FilePath& out)
{
    static constexpr Array sizes = {16, 24, 32, 48, 64, 128, 256};

    const auto limit = maxDimension(src);

    Vector<Bytes> frames;
    Vector<int> frameSizes;
    for (const auto size: sizes)
    {
        if (!keepSize(size, limit, 16))
            continue;

        frames.add(pngFrame(src, size));
        frameSizes.add(size);
    }

    const auto count = static_cast<std::uint16_t>(frames.size());

    Bytes dir;
    appendLE16(dir, 0); // reserved
    appendLE16(dir, 1); // type: icon
    appendLE16(dir, count);

    auto offset = static_cast<std::uint32_t>(6 + 16 * frames.size());
    for (auto i = 0; i < frames.size(); ++i)
    {
        const auto size = frameSizes[i];
        const auto bytesInRes = static_cast<std::uint32_t>(frames[i].size());

        // A 256px side is stored as 0 in the single width/height byte.
        dir.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
        dir.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
        dir.push_back(0); // color count (0 for true colour)
        dir.push_back(0); // reserved
        appendLE16(dir, 1); // colour planes
        appendLE16(dir, 32); // bits per pixel
        appendLE32(dir, bytesInRes);
        appendLE32(dir, offset);
        offset += bytesInRes;
    }

    Bytes file = dir;
    for (const auto& frame: frames)
        file.addFrom(frame);

    Files::writeFile(out, file);
}

void writeIconset(const Image& src, const FilePath& outDir)
{
    const auto png = pngFrame(src, 1024);
    Files::writeFile(outDir / "icon_1024.png", png);

    static constexpr auto contents = "{\n"
                                     "  \"images\" : [\n"
                                     "    {\n"
                                     "      \"filename\" : \"icon_1024.png\",\n"
                                     "      \"idiom\" : \"universal\",\n"
                                     "      \"platform\" : \"ios\",\n"
                                     "      \"size\" : \"1024x1024\"\n"
                                     "    }\n"
                                     "  ],\n"
                                     "  \"info\" : {\n"
                                     "    \"author\" : \"eacp-icon-tool\",\n"
                                     "    \"version\" : 1\n"
                                     "  }\n"
                                     "}\n";

    const std::string json = contents;

    Bytes jsonBytes;
    jsonBytes.assign(json.begin(), json.end());
    Files::writeFile(outDir / "Contents.json", jsonBytes);
}

} // namespace eacp::Graphics::Icons
