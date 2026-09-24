#include "SafetensorsFile.h"

#include "../../GPU/Codegen/PackedVertex.h"
#include "Json.h"

#include <algorithm>
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

// The most a segment spans when it holds more than one tensor. Small enough
// that a checkpoint's unused tail is left out to within a fraction of it and
// that the residency requests run side by side; large enough that a model's
// weights are a few dozen buffers rather than thousands.
constexpr auto segmentTargetBytes = std::int64_t {256} << 20;

std::int64_t pageFloor(std::int64_t offset)
{
    const auto page = GPU::Buffer::memoryPageSize();
    return offset / page * page;
}
} // namespace

SafetensorsFile::SafetensorsFile(
    MemoryMappedFile mappedFile,
    std::uint64_t dataStart,
    std::map<std::string, SafetensorsEntry> entriesToUse)
    : mapped(std::make_shared<const MemoryMappedFile>(std::move(mappedFile)))
    , dataSectionStart(dataStart)
    , entries(std::move(entriesToUse))
{
    auto spans = std::vector<std::pair<std::int64_t, std::int64_t>> {};

    for (const auto& [name, entry]: entries)
        if (entry.dtype == SafetensorsDType::F32 && entry.byteLength > 0)
        {
            auto offset = fileOffsetOf(entry);
            spans.emplace_back(offset, offset + (std::int64_t) entry.byteLength);
        }

    std::sort(spans.begin(), spans.end());

    for (const auto& [begin, end]: spans)
    {
        auto fits =
            !segments.empty() && end - segments.back().start <= segmentTargetBytes;

        if (fits)
            segments.back().end = std::max(segments.back().end, end);
        else
            segments.push_back({begin, pageFloor(begin), end, nullptr, false});
    }
}

std::optional<SafetensorsFile> SafetensorsFile::open(const FilePath& path)
{
    auto mapped = MemoryMappedFile {path};

    if (!mapped.isValid())
        return std::nullopt;

    auto bytes = mapped.bytes();

    if (bytes.getSize() < 8)
        return std::nullopt;

    auto headerLength = std::uint64_t {};
    std::memcpy(&headerLength, bytes.data(), sizeof(headerLength));

    if (headerLength > bytes.getSize() - 8)
        return std::nullopt;

    auto headerText = std::string_view {
        reinterpret_cast<const char*>(bytes.data()) + 8, (std::size_t) headerLength};

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
        entry.dtype = dtypeField != nullptr ? dtypeFromName(dtypeField->asString())
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

    return mapped->bytes().data() + fileOffsetOf(*entry);
}

std::int64_t SafetensorsFile::fileOffsetOf(const SafetensorsEntry& entry) const
{
    return (std::int64_t) (dataSectionStart + entry.byteOffset);
}

SafetensorsFile::Segment&
    SafetensorsFile::segmentHolding(std::int64_t fileOffset) const
{
    auto after = std::upper_bound(segments.begin(),
                                  segments.end(),
                                  fileOffset,
                                  [](std::int64_t offset, const Segment& segment)
                                  { return offset < segment.firstTensorOffset; });

    assert(after != segments.begin());
    return *std::prev(after);
}

// A segment's buffer, made on its first load for a device and shared by every
// tensor in it after that. Null where the device copies instead, or where it
// declined the memory.
std::shared_ptr<const GPU::Buffer>
    SafetensorsFile::bufferFor(Segment& segment, GPU::Device& device) const
{
    if (segmentDevice != &device)
    {
        segmentDevice = &device;

        for (auto& each: segments)
            each = {each.firstTensorOffset, each.start, each.end, nullptr, false};
    }

    if (segment.adopted)
        return segment.buffer;

    segment.adopted = true;

    if (!GPU::Buffer::canAdoptMemory(device))
        return nullptr;

    auto memory = GPU::ExternalMemory {
        const_cast<std::uint8_t*>(mapped->bytes().data() + segment.start),
        segment.end - segment.start,
        [keepMapped = mapped] {}};

    auto buffer = device.makeBufferOverMemory(std::move(memory));

    if (buffer.isValid())
        segment.buffer = std::make_shared<const GPU::Buffer>(std::move(buffer));

    return segment.buffer;
}

int SafetensorsFile::segmentBufferCount() const
{
    return (int) std::count_if(segments.begin(),
                               segments.end(),
                               [](const Segment& segment)
                               { return segment.buffer != nullptr; });
}

Tensor SafetensorsFile::loadF32(const std::string& name, GPU::Device& device) const
{
    auto entry = find(name);
    assert(entry != nullptr && bytesPerElement(entry->dtype) > 0);

    if (entry->dtype != SafetensorsDType::F32)
    {
        ++counts.converted;
        auto values = readF32(name);
        return Tensor::fromHostF32(values.data(), entry->shape, device);
    }

    auto offset = fileOffsetOf(*entry);
    auto onGrid = offset % device.storageBufferOffsetAlignment() == 0;

    if (onGrid && entry->byteLength > 0)
    {
        auto& segment = segmentHolding(offset);

        if (auto buffer = bufferFor(segment, device))
        {
            ++counts.inPlace;
            return Tensor {
                std::move(buffer), offset - segment.start, entry->shape, DType::F32};
        }
    }

    ++counts.copied;
    auto data = reinterpret_cast<const float*>(rawBytes(name));
    return Tensor::fromHostF32(data, entry->shape, device);
}

std::vector<float> SafetensorsFile::readF32(const std::string& name) const
{
    auto entry = find(name);
    assert(entry != nullptr && bytesPerElement(entry->dtype) > 0);

    auto count = (std::size_t) elementCountOf(entry->shape);
    auto bytes = rawBytes(name);
    auto values = std::vector<float>(count);

    if (entry->dtype == SafetensorsDType::F32)
    {
        std::memcpy(values.data(), bytes, count * sizeof(float));
        return values;
    }

    for (auto i = std::size_t {}; i < count; ++i)
    {
        auto bits = std::uint16_t {};
        std::memcpy(&bits, bytes + i * 2, sizeof(bits));

        values[i] = entry->dtype == SafetensorsDType::BF16
                        ? GPU::bfloat16ToFloat(bits)
                        : GPU::halfToFloat(bits);
    }

    return values;
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

    auto values = readF32(name);
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
} // namespace eacp::ML
