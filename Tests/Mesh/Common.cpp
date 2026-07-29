#include "Common.h"

namespace eacp::Mesh::Tests
{
namespace
{
const char* base64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encodeBase64(const std::vector<std::uint8_t>& bytes)
{
    auto result = std::string {};
    result.reserve((bytes.size() + 2) / 3 * 4);

    for (auto i = std::size_t {0}; i < bytes.size(); i += 3)
    {
        auto remaining = bytes.size() - i;

        auto word = (std::uint32_t) bytes[i] << 16;
        word |= (std::uint32_t) (remaining > 1 ? bytes[i + 1] : 0) << 8;
        word |= (std::uint32_t) (remaining > 2 ? bytes[i + 2] : 0);

        result += base64Alphabet[(word >> 18) & 0x3f];
        result += base64Alphabet[(word >> 12) & 0x3f];
        result += remaining > 1 ? base64Alphabet[(word >> 6) & 0x3f] : '=';
        result += remaining > 2 ? base64Alphabet[word & 0x3f] : '=';
    }

    return result;
}

void replaceAll(std::string& text, const std::string& token, const std::string& with)
{
    for (auto at = text.find(token); at != std::string::npos;
         at = text.find(token, at + with.size()))
        text.replace(at, token.size(), with);
}
} // namespace

std::string BinaryBuffer::asDataUri() const
{
    return "data:application/octet-stream;base64," + encodeBase64(bytes);
}

LoadResult loadDocument(const std::string& json, const BinaryBuffer& binary)
{
    auto document = json;

    replaceAll(document, "@BUFFER@", binary.asDataUri());
    replaceAll(document, "@BYTELENGTH@", std::to_string(binary.size()));

    return loadGltfFromMemory(document.data(), document.size());
}

std::vector<float> cubePositions()
{
    // Four corners per face rather than eight shared ones, because each face
    // wants its own normal - and a test that generates normals for this needs
    // the faces not to share vertices, or every corner averages three faces.
    return {// +z
            -1,
            -1,
            1,
            1,
            -1,
            1,
            1,
            1,
            1,
            -1,
            1,
            1,
            // -z
            1,
            -1,
            -1,
            -1,
            -1,
            -1,
            -1,
            1,
            -1,
            1,
            1,
            -1,
            // +x
            1,
            -1,
            1,
            1,
            -1,
            -1,
            1,
            1,
            -1,
            1,
            1,
            1,
            // -x
            -1,
            -1,
            -1,
            -1,
            -1,
            1,
            -1,
            1,
            1,
            -1,
            1,
            -1,
            // +y
            -1,
            1,
            1,
            1,
            1,
            1,
            1,
            1,
            -1,
            -1,
            1,
            -1,
            // -y
            -1,
            -1,
            -1,
            1,
            -1,
            -1,
            1,
            -1,
            1,
            -1,
            -1,
            1};
}

std::vector<std::uint16_t> cubeIndices()
{
    auto indices = std::vector<std::uint16_t> {};

    for (auto face = 0; face < 6; ++face)
    {
        auto base = (std::uint16_t) (face * 4);

        indices.insert(indices.end(),
                       {base,
                        (std::uint16_t) (base + 1),
                        (std::uint16_t) (base + 2),
                        base,
                        (std::uint16_t) (base + 2),
                        (std::uint16_t) (base + 3)});
    }

    return indices;
}
} // namespace eacp::Mesh::Tests
