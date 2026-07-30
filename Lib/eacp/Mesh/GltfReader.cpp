#include "GltfReader.h"

#include <eacp/Core/Utils/Files.h>

#include <algorithm>
#include <cstring>

namespace eacp::Mesh::Gltf
{
namespace
{
// Reads a little-endian value out of possibly-unaligned bytes. glTF is
// little-endian throughout and a buffer view may start anywhere, so every read
// of the binary goes through this rather than a cast - which would be undefined
// on the offsets a real file produces.
template <typename T>
T readLittleEndian(const std::uint8_t* source)
{
    auto value = T {};
    std::memcpy(&value, source, sizeof(T));
    return value;
}

// A signed normalized value maps -1..1, and the negative end has one more code
// than the positive: -128 and -32768 both divide to slightly below -1, which the
// spec says to clamp rather than let through.
float normalizeSigned(float value, float scale)
{
    return std::max(value / scale, -1.0f);
}

int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;

    return -1;
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

constexpr std::uint32_t glbMagic = 0x46546C67; // "glTF"
constexpr std::uint32_t glbJsonChunk = 0x4E4F534A; // "JSON"
constexpr std::uint32_t glbBinaryChunk = 0x004E4942; // "BIN\0"
constexpr auto glbHeaderSize = 12;
constexpr auto glbChunkHeaderSize = 8;
} // namespace

const Miro::Json::Value* member(const Miro::Json::Value& value, std::string_view key)
{
    if (!value.isObject())
        return nullptr;

    return Miro::Json::find(value.asObject(), key);
}

int intOr(const Miro::Json::Value& value, std::string_view key, int fallback)
{
    const auto* found = member(value, key);
    return found != nullptr && found->isNumber() ? (int) found->asNumber()
                                                 : fallback;
}

float floatOr(const Miro::Json::Value& value, std::string_view key, float fallback)
{
    const auto* found = member(value, key);
    return found != nullptr && found->isNumber() ? (float) found->asNumber()
                                                 : fallback;
}

bool boolOr(const Miro::Json::Value& value, std::string_view key, bool fallback)
{
    const auto* found = member(value, key);
    return found != nullptr && found->isBool() ? found->asBool() : fallback;
}

std::string stringOr(const Miro::Json::Value& value,
                     std::string_view key,
                     std::string_view fallback)
{
    const auto* found = member(value, key);

    if (found != nullptr && found->isString())
        return found->asString();

    return std::string {fallback};
}

const Miro::Json::Array* arrayMember(const Miro::Json::Value& value,
                                     std::string_view key)
{
    const auto* found = member(value, key);
    return found != nullptr && found->isArray() ? &found->asArray() : nullptr;
}

const Miro::Json::Value* at(const Miro::Json::Array* array, int index)
{
    if (array == nullptr || index < 0 || index >= array->size())
        return nullptr;

    return &array->get(index);
}

int readNumbers(const Miro::Json::Value& value,
                std::string_view key,
                float* out,
                int count)
{
    const auto* array = arrayMember(value, key);

    if (array == nullptr)
        return 0;

    auto written = 0;

    for (auto i = 0; i < count && i < array->size(); ++i)
    {
        const auto& element = array->get(i);

        if (!element.isNumber())
            break;

        out[i] = (float) element.asNumber();
        ++written;
    }

    return written;
}

int bytesPerComponent(ComponentType type)
{
    switch (type)
    {
        case ComponentType::Byte:
        case ComponentType::UnsignedByte:
            return 1;
        case ComponentType::Short:
        case ComponentType::UnsignedShort:
            return 2;
        case ComponentType::UnsignedInt:
        case ComponentType::Float:
            return 4;
        default:
            return 0;
    }
}

int componentsForType(std::string_view type)
{
    if (type == "SCALAR")
        return 1;
    if (type == "VEC2")
        return 2;
    if (type == "VEC3")
        return 3;
    if (type == "VEC4")
        return 4;

    // MAT2, MAT3 and MAT4 land here deliberately. No attribute this reader
    // consumes is a matrix, and byte- and short-component matrices pad every
    // column to four bytes - a rule that is invisible until it silently skews a
    // mesh, so refusing is better than a half-implementation.
    return 0;
}

float FloatData::get(int element, int component) const
{
    if (element < 0 || element >= count || component < 0 || component >= components)
        return 0.0f;

    return values[element * components + component];
}

Vector<std::uint8_t> decodeBase64(std::string_view encoded)
{
    auto output = Vector<std::uint8_t> {};

    // Strip the trailing padding first, so the loop below never has to treat '='
    // as a value and the expected output size is arithmetic rather than a guess.
    while (!encoded.empty() && encoded.back() == '=')
        encoded.remove_suffix(1);

    output.reserve((int) (encoded.size() / 4 * 3 + 3));

    auto accumulator = std::uint32_t {0};
    auto bits = 0;

    for (auto c: encoded)
    {
        // Whitespace inside a base64 payload is legal in the wild and harmless.
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;

        auto value = base64Value(c);

        if (value < 0)
            return {};

        accumulator = (accumulator << 6) | (std::uint32_t) value;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            output.add((std::uint8_t) ((accumulator >> bits) & 0xff));
        }
    }

    return output;
}

std::string decodePercentEscapes(std::string_view uri)
{
    auto result = std::string {};
    result.reserve(uri.size());

    for (auto i = std::size_t {0}; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size())
        {
            auto high = hexValue(uri[i + 1]);
            auto low = hexValue(uri[i + 2]);

            if (high >= 0 && low >= 0)
            {
                result += (char) (high * 16 + low);
                i += 2;
                continue;
            }
        }

        result += uri[i];
    }

    return result;
}

bool Document::splitGlb(std::span<const std::uint8_t> bytes,
                        std::string_view& json,
                        std::span<const std::uint8_t>& binary)
{
    if (bytes.size() < glbHeaderSize)
    {
        errorText = "the file is too short to be a GLB";
        return false;
    }

    auto declared = readLittleEndian<std::uint32_t>(bytes.data() + 8);

    // The header's own length field, honoured rather than trusted: a file padded
    // by a transport is common, one that declares more than it has is corrupt.
    if (declared > bytes.size())
    {
        errorText = "the GLB declares more bytes than the file holds";
        return false;
    }

    auto end = (std::size_t) declared;
    auto offset = (std::size_t) glbHeaderSize;

    while (offset + glbChunkHeaderSize <= end)
    {
        auto length = readLittleEndian<std::uint32_t>(bytes.data() + offset);
        auto type = readLittleEndian<std::uint32_t>(bytes.data() + offset + 4);

        offset += glbChunkHeaderSize;

        if (offset + length > end)
        {
            errorText = "a GLB chunk runs past the end of the file";
            return false;
        }

        if (type == glbJsonChunk)
            json = std::string_view {(const char*) bytes.data() + offset, length};
        else if (type == glbBinaryChunk)
            binary = bytes.subspan(offset, length);

        // Chunks are four-byte aligned, and an unknown type is skipped rather
        // than refused - the spec says a reader must ignore ones it does not
        // know.
        offset += (length + 3) & ~std::size_t {3};
    }

    if (json.empty())
    {
        errorText = "the GLB has no JSON chunk";
        return false;
    }

    return true;
}

bool Document::parse(std::span<const std::uint8_t> bytes, const FilePath& basePath)
{
    base = basePath;

    auto json = std::string_view {(const char*) bytes.data(), bytes.size()};
    auto glbBinary = std::span<const std::uint8_t> {};

    // GLB and glTF are told apart by the magic word rather than by a file
    // extension, so bytes handed over with no name still load.
    if (bytes.size() >= 4
        && readLittleEndian<std::uint32_t>(bytes.data()) == glbMagic)
        if (!splitGlb(bytes, json, glbBinary))
            return false;

    try
    {
        rootValue = Miro::Json::parse(json);
    }
    catch (const std::exception& e)
    {
        errorText = std::string {"the JSON is malformed: "} + e.what();
        return false;
    }

    if (!rootValue.isObject())
    {
        errorText = "the top level of a glTF is an object, and this is not";
        return false;
    }

    // glTF 1.0 is a different format with the same extension. It has no
    // "asset.version" of 2, and reading it as 2.0 produces nothing useful.
    const auto* asset = member(rootValue, "asset");

    if (asset == nullptr)
    {
        errorText = "no asset block, so this is not a glTF 2.0 file";
        return false;
    }

    auto version = stringOr(*asset, "version");

    if (!version.starts_with("2."))
    {
        errorText = "glTF version " + version + ", and this reader is 2.0 only";
        return false;
    }

    if (!resolveBuffers(glbBinary))
        return false;

    resolveViews();

    return resolveAccessors();
}

bool Document::resolveBuffers(std::span<const std::uint8_t> glbBinary)
{
    const auto* declared = collection("buffers");

    if (declared == nullptr)
        return true;

    for (auto i = 0; i < declared->size(); ++i)
    {
        const auto& entry = declared->get(i);
        auto bytes = Vector<std::uint8_t> {};

        const auto* uri = member(entry, "uri");

        if (uri == nullptr || !uri->isString())
        {
            // No URI means the GLB binary chunk, which only buffer 0 may claim.
            if (i == 0 && !glbBinary.empty())
                bytes.getVector().assign(glbBinary.begin(), glbBinary.end());
        }
        else
        {
            bytes = readUri(uri->asString());
        }

        // A declared byteLength longer than what arrived is the one case worth
        // refusing outright: every accessor bound-checks against the view, and a
        // short buffer would make those checks pass against data that is not
        // there.
        auto byteLength = intOr(entry, "byteLength", 0);

        if (byteLength > 0 && bytes.size() < byteLength)
        {
            errorText =
                "buffer " + std::to_string(i) + " is shorter than it declares";
            return false;
        }

        buffers.add(std::move(bytes));
    }

    return true;
}

void Document::resolveViews()
{
    const auto* declared = collection("bufferViews");

    if (declared == nullptr)
        return;

    for (auto i = 0; i < declared->size(); ++i)
    {
        const auto& entry = declared->get(i);

        auto view = BufferView {};
        view.buffer = intOr(entry, "buffer", -1);
        view.byteOffset = intOr(entry, "byteOffset", 0);
        view.byteLength = intOr(entry, "byteLength", 0);
        view.byteStride = intOr(entry, "byteStride", 0);

        views.add(view);
    }
}

bool Document::resolveAccessors()
{
    const auto* declared = collection("accessors");

    if (declared == nullptr)
        return true;

    for (auto i = 0; i < declared->size(); ++i)
    {
        const auto& entry = declared->get(i);

        auto accessor = Accessor {};
        accessor.bufferView = intOr(entry, "bufferView", -1);
        accessor.byteOffset = intOr(entry, "byteOffset", 0);
        accessor.componentType = (ComponentType) intOr(entry, "componentType", 0);
        accessor.normalized = boolOr(entry, "normalized", false);
        accessor.count = intOr(entry, "count", 0);
        accessor.components = componentsForType(stringOr(entry, "type"));

        if (const auto* sparse = member(entry, "sparse"); sparse != nullptr)
        {
            const auto* indices = member(*sparse, "indices");
            const auto* values = member(*sparse, "values");

            if (indices != nullptr && values != nullptr)
            {
                accessor.hasSparse = true;
                accessor.sparseCount = intOr(*sparse, "count", 0);
                accessor.sparseIndexView = intOr(*indices, "bufferView", -1);
                accessor.sparseIndexOffset = intOr(*indices, "byteOffset", 0);
                accessor.sparseIndexType =
                    (ComponentType) intOr(*indices, "componentType", 0);
                accessor.sparseValueView = intOr(*values, "bufferView", -1);
                accessor.sparseValueOffset = intOr(*values, "byteOffset", 0);
            }
        }

        accessors.add(accessor);
    }

    return true;
}

const Miro::Json::Array* Document::collection(std::string_view key) const
{
    return arrayMember(rootValue, key);
}

std::span<const std::uint8_t> Document::bufferViewBytes(int index) const
{
    if (index < 0 || index >= views.size())
        return {};

    const auto& view = views[index];

    if (view.buffer < 0 || view.buffer >= buffers.size())
        return {};

    const auto& buffer = buffers[view.buffer];

    if (view.byteOffset < 0 || view.byteLength < 0
        || view.byteOffset + view.byteLength > buffer.size())
        return {};

    return std::span<const std::uint8_t> {buffer.data() + view.byteOffset,
                                          (std::size_t) view.byteLength};
}

Vector<std::uint8_t> Document::readUri(std::string_view uri) const
{
    if (uri.starts_with("data:"))
    {
        auto comma = uri.find(',');

        if (comma == std::string_view::npos)
            return {};

        auto payload = uri.substr(comma + 1);

        // Only base64 payloads. A percent-encoded data URI is legal and
        // vanishingly rare, and decoding one as base64 would produce garbage
        // rather than nothing.
        if (uri.substr(0, comma).find(";base64") == std::string_view::npos)
            return {};

        return decodeBase64(payload);
    }

    if (base.empty())
        return {};

    auto contents = Files::readFile(base / decodePercentEscapes(uri));

    auto bytes = Vector<std::uint8_t> {};
    bytes.getVector().assign(contents.begin(), contents.end());
    return bytes;
}

bool Document::readElements(const Accessor& accessor,
                            int viewIndex,
                            int byteOffset,
                            int firstElement,
                            int elementCount,
                            int maxComponents,
                            float* out) const
{
    auto bytes = bufferViewBytes(viewIndex);

    if (bytes.empty())
        return false;

    auto componentSize = bytesPerComponent(accessor.componentType);

    if (componentSize == 0 || accessor.components == 0)
        return false;

    auto tight = accessor.components * componentSize;

    // A view may declare a stride, which is how interleaved vertex data is laid
    // out; without one the elements are tightly packed. A stride smaller than one
    // element would make elements overlap, which no valid file does.
    auto stride =
        views[viewIndex].byteStride > 0 ? views[viewIndex].byteStride : tight;

    if (stride < tight)
        return false;

    auto kept = std::min(accessor.components, maxComponents);

    for (auto element = 0; element < elementCount; ++element)
    {
        auto elementStart =
            (std::size_t) byteOffset
            + (std::size_t) (firstElement + element) * (std::size_t) stride;

        // Bounds-checked per element rather than once up front, because the last
        // element of a strided view need only hold its own components - the
        // stride's tail may legitimately run off the end.
        if (elementStart + (std::size_t) tight > bytes.size())
            return false;

        for (auto component = 0; component < kept; ++component)
        {
            const auto* source =
                bytes.data() + elementStart
                + (std::size_t) component * (std::size_t) componentSize;

            auto value = 0.0f;

            switch (accessor.componentType)
            {
                case ComponentType::Byte:
                {
                    auto raw = (float) readLittleEndian<std::int8_t>(source);
                    value = accessor.normalized ? normalizeSigned(raw, 127.0f) : raw;
                    break;
                }
                case ComponentType::UnsignedByte:
                {
                    auto raw = (float) readLittleEndian<std::uint8_t>(source);
                    value = accessor.normalized ? raw / 255.0f : raw;
                    break;
                }
                case ComponentType::Short:
                {
                    auto raw = (float) readLittleEndian<std::int16_t>(source);
                    value =
                        accessor.normalized ? normalizeSigned(raw, 32767.0f) : raw;
                    break;
                }
                case ComponentType::UnsignedShort:
                {
                    auto raw = (float) readLittleEndian<std::uint16_t>(source);
                    value = accessor.normalized ? raw / 65535.0f : raw;
                    break;
                }
                case ComponentType::UnsignedInt:
                    value = (float) readLittleEndian<std::uint32_t>(source);
                    break;
                case ComponentType::Float:
                    value = readLittleEndian<float>(source);
                    break;
                default:
                    return false;
            }

            out[element * kept + component] = value;
        }
    }

    return true;
}

void Document::applySparse(const Accessor& accessor,
                           int maxComponents,
                           FloatData& data) const
{
    auto indexBytes = bufferViewBytes(accessor.sparseIndexView);
    auto indexSize = bytesPerComponent(accessor.sparseIndexType);

    if (indexBytes.empty() || indexSize == 0)
        return;

    auto kept = data.components;

    // The overriding values are always tightly packed and of the accessor's own
    // component type, whatever stride the dense view used.
    auto replacement = Vector<float> {};
    replacement.resize(accessor.sparseCount * kept);

    auto values = accessor;
    values.hasSparse = false;

    if (!readElements(values,
                      accessor.sparseValueView,
                      accessor.sparseValueOffset,
                      0,
                      accessor.sparseCount,
                      maxComponents,
                      replacement.data()))
        return;

    for (auto i = 0; i < accessor.sparseCount; ++i)
    {
        auto offset = (std::size_t) accessor.sparseIndexOffset
                      + (std::size_t) i * (std::size_t) indexSize;

        if (offset + (std::size_t) indexSize > indexBytes.size())
            return;

        const auto* source = indexBytes.data() + offset;
        auto target = 0;

        switch (accessor.sparseIndexType)
        {
            case ComponentType::UnsignedByte:
                target = readLittleEndian<std::uint8_t>(source);
                break;
            case ComponentType::UnsignedShort:
                target = readLittleEndian<std::uint16_t>(source);
                break;
            case ComponentType::UnsignedInt:
                target = (int) readLittleEndian<std::uint32_t>(source);
                break;
            default:
                return;
        }

        if (target < 0 || target >= data.count)
            continue;

        for (auto component = 0; component < kept; ++component)
            data.values[target * kept + component] =
                replacement[i * kept + component];
    }
}

FloatData Document::readFloats(int index, int maxComponents) const
{
    if (index < 0 || index >= accessors.size())
        return {};

    const auto& accessor = accessors[index];

    if (accessor.count <= 0 || accessor.components == 0)
        return {};

    auto data = FloatData {};
    data.count = accessor.count;
    data.components = std::min(accessor.components, maxComponents);

    data.values.resize(data.count * data.components);

    // An accessor with no bufferView is all zeroes by definition, which sparse
    // then overrides - the shape a file uses to send a handful of displaced
    // vertices and nothing else.
    if (accessor.bufferView >= 0)
        if (!readElements(accessor,
                          accessor.bufferView,
                          accessor.byteOffset,
                          0,
                          accessor.count,
                          maxComponents,
                          data.values.data()))
            return {};

    if (accessor.hasSparse)
        applySparse(accessor, maxComponents, data);

    return data;
}

Vector<std::uint32_t> Document::readIndices(int index) const
{
    auto data = readFloats(index, 1);

    if (!data.isValid())
        return {};

    // Through float is exact here and not a shortcut: glTF indices are at most
    // 32-bit unsigned, and a float carries 24 bits of mantissa - which covers
    // every index a 16-bit-per-primitive mesh can hold and every one this
    // renderer will draw. A model with more than 16 million vertices in a single
    // primitive would lose precision, and would also be refused by
    // fitsNarrowIndices long before that mattered.
    auto result = Vector<std::uint32_t> {};
    result.reserve(data.count);

    for (auto i = 0; i < data.count; ++i)
        result.add((std::uint32_t) data.get(i, 0));

    return result;
}
} // namespace eacp::Mesh::Gltf
