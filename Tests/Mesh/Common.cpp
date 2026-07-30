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

namespace
{
void appendWord(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back((std::uint8_t) (value & 0xff));
    out.push_back((std::uint8_t) ((value >> 8) & 0xff));
    out.push_back((std::uint8_t) ((value >> 16) & 0xff));
    out.push_back((std::uint8_t) ((value >> 24) & 0xff));
}

// A chunk's payload is padded to a four-byte boundary, and the pad byte differs
// by type: a JSON chunk pads with spaces so the text stays parseable, a BIN chunk
// with zeroes. The declared length is the unpadded one.
void appendChunk(std::vector<std::uint8_t>& out,
                 std::uint32_t type,
                 const std::uint8_t* payload,
                 std::size_t size,
                 std::uint8_t padByte)
{
    auto padded = (size + 3) & ~std::size_t {3};

    appendWord(out, (std::uint32_t) size);
    appendWord(out, type);

    out.insert(out.end(), payload, payload + size);
    out.insert(out.end(), padded - size, padByte);
}
} // namespace

std::vector<std::uint8_t> makeGlb(const std::string& json,
                                  const std::vector<std::uint8_t>& binary)
{
    constexpr std::uint32_t magic = 0x46546C67; // "glTF"
    constexpr std::uint32_t jsonChunk = 0x4E4F534A; // "JSON"
    constexpr std::uint32_t binaryChunk = 0x004E4942; // "BIN\0"

    auto chunks = std::vector<std::uint8_t> {};

    appendChunk(chunks,
                jsonChunk,
                reinterpret_cast<const std::uint8_t*>(json.data()),
                json.size(),
                ' ');

    if (!binary.empty())
        appendChunk(chunks, binaryChunk, binary.data(), binary.size(), 0);

    auto glb = std::vector<std::uint8_t> {};

    appendWord(glb, magic);
    appendWord(glb, 2);

    // The total length includes the header, which is why it is written after the
    // chunks are sized rather than guessed at.
    appendWord(glb, (std::uint32_t) (12 + chunks.size()));

    glb.insert(glb.end(), chunks.begin(), chunks.end());
    return glb;
}

LoadResult loadBytes(const std::vector<std::uint8_t>& bytes)
{
    return loadGltfFromMemory(bytes.data(), bytes.size());
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
