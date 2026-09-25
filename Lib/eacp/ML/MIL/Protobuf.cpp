#include "Protobuf.h"

#include <bit>

namespace eacp::ML::Protobuf
{
void Writer::varint(std::uint64_t value)
{
    while (value >= 0x80)
    {
        out.add(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }

    out.add(static_cast<std::uint8_t>(value));
}

void Writer::tag(int field, WireType type)
{
    varint((static_cast<std::uint64_t>(field) << 3)
           | static_cast<std::uint64_t>(type));
}

void Writer::uint64Field(int field, std::uint64_t value)
{
    if (value == 0)
        return;

    tag(field, WireType::varint);
    varint(value);
}

void Writer::int64Field(int field, std::int64_t value)
{
    uint64Field(field, static_cast<std::uint64_t>(value));
}

void Writer::boolField(int field, bool value)
{
    uint64Field(field, value ? 1 : 0);
}

void Writer::bytesField(int field, Span<const std::uint8_t> value)
{
    tag(field, WireType::lengthDelimited);
    varint(value.getSize());
    append(value);
}

void Writer::stringField(int field, std::string_view value)
{
    bytesField(field, asBytes(value));
}

void Writer::messageField(int field, const Writer& message)
{
    bytesField(field, message.out);
}

void Writer::mapEntry(int field, std::string_view key, const Writer& value)
{
    auto entry = Writer {};
    entry.stringField(1, key);
    entry.messageField(2, value);
    messageField(field, entry);
}

void Writer::packedInt64(int field, Span<const std::int64_t> values)
{
    auto packed = Writer {};

    for (auto value: values)
        packed.varint(static_cast<std::uint64_t>(value));

    packedField(field, packed);
}

void Writer::packedInt32(int field, Span<const std::int32_t> values)
{
    auto packed = Writer {};

    for (auto value: values)
        packed.varint(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));

    packedField(field, packed);
}

void Writer::packedBool(int field, Span<const std::uint8_t> values)
{
    auto packed = Writer {};

    for (auto value: values)
        packed.varint(value != 0 ? 1 : 0);

    packedField(field, packed);
}

void Writer::packedFloat(int field, Span<const float> values)
{
    auto packed = Writer {};

    for (auto value: values)
    {
        auto bits = std::bit_cast<std::uint32_t>(value);

        for (auto shift = 0; shift < 32; shift += 8)
            packed.out.add(static_cast<std::uint8_t>(bits >> shift));
    }

    packedField(field, packed);
}

void Writer::append(Span<const std::uint8_t> data)
{
    out.getVector().insert(out.end(), data.begin(), data.end());
}

void Writer::packedField(int field, const Writer& packed)
{
    if (!packed.out.empty())
        messageField(field, packed);
}
} // namespace eacp::ML::Protobuf
