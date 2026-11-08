#include "ShaderStruct.h"

#include "PackedScalars.h"

#include <bit>
#include <cstring>

namespace eacp::GPU::detail
{
void writeStorageValue(std::byte* at, ValueType type, const void* components)
{
    const auto* words = static_cast<const std::uint32_t*>(components);

    for (auto component = 0; component < componentCount(type); ++component)
    {
        auto* target = at + storageComponentOffset(type, component);

        if (isHalf(type))
        {
            auto bits = halfFromFloat(std::bit_cast<float>(words[component]));
            std::memcpy(target, &bits, sizeof(bits));
        }
        else
        {
            std::memcpy(target, words + component, sizeof(std::uint32_t));
        }
    }
}

void readStorageValue(const std::byte* at, ValueType type, void* components)
{
    auto* words = static_cast<std::uint32_t*>(components);

    for (auto component = 0; component < componentCount(type); ++component)
    {
        const auto* source = at + storageComponentOffset(type, component);

        if (isHalf(type))
        {
            auto bits = std::uint16_t {};
            std::memcpy(&bits, source, sizeof(bits));
            words[component] = std::bit_cast<std::uint32_t>(halfToFloat(bits));
        }
        else
        {
            std::memcpy(words + component, source, sizeof(std::uint32_t));
        }
    }
}
} // namespace eacp::GPU::detail
