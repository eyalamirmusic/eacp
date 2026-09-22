#include "SafetensorsFile.h"

#include "../../GPU/Codegen/PackedVertex.h"
#include "Json.h"

#include <cassert>
#include <cstring>

namespace eacp::ML
{
namespace
{
SafetensorsDType dtypeFromName(const std::string& name)
{
    if (name == "F32")
        return SafetensorsDType::F32;

    if (name == "F16")
        return SafetensorsDType::F16;

    if (name == "BF16")
        return SafetensorsDType::BF16;

    return SafetensorsDType::Unknown;
}

int bytesPerElement(SafetensorsDType dtype)
{
    switch (dtype)
    {
        case SafetensorsDType::F32:
            return 4;
        case SafetensorsDType::F16:
        case SafetensorsDType::BF16:
            return 2;
        case SafetensorsDType::Unknown:
            return 0;
    }

    return 0;
}
}

SafetensorsFile::SafetensorsFile(MemoryMappedFile mappedFile,
                                 std::uint64_t dataStart,
                                 std::map<std::string, SafetensorsEntry> entriesToUse)
    : mapped(std::move(mappedFile))
    , dataSectionStart(dataStart)
    , entries(std::move(entriesToUse))
{
}

std::optional<SafetensorsFile> SafetensorsFile::open(const FilePath& path)
{
    auto mapped = MemoryMappedFile {path};

    if (!mapped.isValid())
        return std::nullopt;

    auto bytes = mapped.bytes();

    if (bytes.size() < 8)
        return std::nullopt;

    auto headerLength = std::uint64_t {};
    std::memcpy(&headerLength, bytes.data(), sizeof(headerLength));

    if (headerLength > (std::uint64_t) bytes.size() - 8)
        return std::nullopt;

    auto headerText =
        std::string_view {reinterpret_cast<const char*>(bytes.data()) + 8,
                          (std::size_t) headerLength};

    auto parsed = Json::parse(headerText);

    if (!parsed.has_value() || !parsed->isObject())
        return std::nullopt;

    auto dataStart = std::uint64_t {8} + headerLength;
    auto entries = std::map<std::string, SafetensorsEntry> {};

    for (const auto& [key, value]: parsed->asObject())
    {
        if (key == "__metadata__" || !value.isObject())
            continue;

        auto entry = SafetensorsEntry {};

        auto dtypeField = value.find("dtype");
        entry.dtype =
            dtypeField != nullptr ? dtypeFromName(dtypeField->asString())
                                  : SafetensorsDType::Unknown;

        auto shapeField = value.find("shape");

        if (shapeField != nullptr)
            for (const auto& extent: shapeField->asArray())
                entry.shape.push_back((int) extent.asNumber());

        auto offsetsField = value.find("data_offsets");
        const auto& offsets =
            offsetsField != nullptr ? offsetsField->asArray() : Json::Array {};

        if (offsets.size() == 2)
        {
            auto start = (std::uint64_t) offsets[0].asNumber();
            auto end = (std::uint64_t) offsets[1].asNumber();
            entry.byteOffset = start;
            entry.byteLength = end - start;
        }

        entries.emplace(key, std::move(entry));
    }

    return SafetensorsFile {std::move(mapped), dataStart, std::move(entries)};
}

const SafetensorsEntry* SafetensorsFile::find(const std::string& name) const
{
    auto found = entries.find(name);
    return found == entries.end() ? nullptr : &found->second;
}

const std::uint8_t* SafetensorsFile::rawBytes(const std::string& name) const
{
    auto entry = find(name);

    if (entry == nullptr)
        return nullptr;

    return mapped.bytes().data() + dataSectionStart + entry->byteOffset;
}

Tensor SafetensorsFile::loadF32(const std::string& name, GPU::Device& device) const
{
    auto entry = find(name);
    assert(entry != nullptr && entry->dtype == SafetensorsDType::F32);

    auto data = reinterpret_cast<const float*>(rawBytes(name));
    return Tensor::fromHostF32(data, entry->shape, device);
}

Tensor SafetensorsFile::loadPackedF16(const std::string& name,
                                      GPU::Device& device) const
{
    auto entry = find(name);
    assert(entry != nullptr);

    if (entry->dtype == SafetensorsDType::F32)
    {
        auto data = reinterpret_cast<const float*>(rawBytes(name));
        return Tensor::fromHostPackedF16(data, entry->shape, device);
    }

    auto count = elementCountOf(entry->shape);
    auto values = std::vector<float> ((std::size_t) count);
    auto bytes = rawBytes(name);

    for (auto i = 0; i < count; ++i)
    {
        auto bits = std::uint16_t {};
        std::memcpy(&bits, bytes + i * 2, sizeof(bits));

        values[(std::size_t) i] = entry->dtype == SafetensorsDType::BF16
                                      ? GPU::bfloat16ToFloat(bits)
                                      : GPU::halfToFloat(bits);
    }

    return Tensor::fromHostPackedF16(values.data(), entry->shape, device);
}

float SafetensorsFile::loadScalar(const std::string& name) const
{
    auto entry = find(name);
    assert(entry != nullptr && bytesPerElement(entry->dtype) > 0);

    if (entry->dtype == SafetensorsDType::F32)
    {
        auto value = float {};
        std::memcpy(&value, rawBytes(name), sizeof(value));
        return value;
    }

    auto bits = std::uint16_t {};
    std::memcpy(&bits, rawBytes(name), sizeof(bits));

    return entry->dtype == SafetensorsDType::BF16 ? GPU::bfloat16ToFloat(bits)
                                                   : GPU::halfToFloat(bits);
}
}
