#pragma once

#include "../Common.h"

#include <concepts>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace eacp::ML
{
using Bytes = Vector<std::uint8_t>;

inline Span<const std::uint8_t> asBytes(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

template <typename Container>
    requires(!std::convertible_to<const Container&, std::string_view>)
Span<const std::uint8_t> asBytes(const Container& values)
{
    auto* first = reinterpret_cast<const std::uint8_t*>(std::data(values));
    auto count = static_cast<std::size_t>(std::size(values));
    return {first, count * sizeof(*std::data(values))};
}
} // namespace eacp::ML

// The protobuf wire format, as much of it as Model.proto and MIL.proto use.
// Fields are written in the order the caller writes them; proto3 scalars at
// their default value are omitted, messages never are.
namespace eacp::ML::Protobuf
{
enum class WireType : std::uint8_t
{
    varint = 0,
    fixed64 = 1,
    lengthDelimited = 2,
    fixed32 = 5
};

class Writer
{
public:
    void varint(std::uint64_t value);
    void tag(int field, WireType type);

    void uint64Field(int field, std::uint64_t value);
    void int64Field(int field, std::int64_t value);
    void boolField(int field, bool value);

    void bytesField(int field, Span<const std::uint8_t> value);
    void stringField(int field, std::string_view value);
    void messageField(int field, const Writer& message);
    void mapEntry(int field, std::string_view key, const Writer& value);

    void packedInt64(int field, Span<const std::int64_t> values);
    void packedInt32(int field, Span<const std::int32_t> values);
    void packedBool(int field, Span<const std::uint8_t> values);
    void packedFloat(int field, Span<const float> values);

    const Bytes& bytes() const { return out; }

private:
    void append(Span<const std::uint8_t> data);
    void packedField(int field, const Writer& packed);

    Bytes out;
};
} // namespace eacp::ML::Protobuf
