#pragma once

#include <Miro/Reflection/ReflectEnum.h>

#include <algorithm>
#include <array>

namespace eacp::GPU
{
template <typename PortableFormat, typename NativeFormat>
struct NativeFormatMapping
{
    PortableFormat portable;
    NativeFormat native;
};

template <typename PortableFormat, typename NativeFormat, std::size_t Size>
using NativeFormatTable =
    std::array<NativeFormatMapping<PortableFormat, NativeFormat>, Size>;

template <typename PortableFormat, typename NativeFormat, std::size_t Size>
constexpr bool mapsEveryFormatOnce(
    const NativeFormatTable<PortableFormat, NativeFormat, Size>& table)
{
    using Mapping = NativeFormatMapping<PortableFormat, NativeFormat>;

    auto namedFormatCount = std::size_t {0};

    for (auto& [format, name]: Miro::Detail::enumTable<PortableFormat>)
    {
        if (name.empty())
            continue;

        ++namedFormatCount;

        if (std::ranges::count(table, format, &Mapping::portable) != 1)
            return false;
    }

    return namedFormatCount == Size;
}

template <typename PortableFormat, typename NativeFormat, std::size_t Size>
constexpr NativeFormat findNativeFormat(
    const NativeFormatTable<PortableFormat, NativeFormat, Size>& table,
    PortableFormat format,
    NativeFormat formatWhenUnmapped)
{
    for (auto& mapping: table)
        if (mapping.portable == format)
            return mapping.native;

    return formatWhenUnmapped;
}
} // namespace eacp::GPU
