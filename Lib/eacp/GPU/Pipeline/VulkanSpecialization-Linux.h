#pragma once

#include "../Shader/PipelineConstants.h"
#include "../Vulkan/VulkanTypes.h"

#include <cstdint>

namespace eacp::GPU
{
// A pipeline's constants as Vulkan specialization info: a map entry per
// constant at its constant_id, each value in a four-byte slot of its own - a
// VkBool32 for a Bool, the binary16 in the slot's first two bytes for a Half.
// The info points into this object, which is why it is neither copied nor
// moved, and get() is null for an empty set, which leaves the module's
// defaults as they are.
class VulkanSpecialization
{
public:
    explicit VulkanSpecialization(const PipelineConstants& constants)
    {
        for (const auto& constant: constants.values)
            add(constant);

        info.mapEntryCount = static_cast<std::uint32_t>(entries.size());
        info.pMapEntries = entries.data();
        info.dataSize = words.getSize() * sizeof(std::uint32_t);
        info.pData = words.data();
    }

    VulkanSpecialization(const VulkanSpecialization&) = delete;
    VulkanSpecialization& operator=(const VulkanSpecialization&) = delete;

    const VkSpecializationInfo* get() const
    {
        return entries.empty() ? nullptr : &info;
    }

private:
    void add(const PipelineConstant& constant)
    {
        const auto isHalf = constant.type == PipelineConstantType::Half;

        auto entry = VkSpecializationMapEntry {};
        entry.constantID = static_cast<std::uint32_t>(constant.id);
        entry.offset =
            static_cast<std::uint32_t>(words.getSize() * sizeof(std::uint32_t));
        entry.size = isHalf ? sizeof(std::uint16_t) : sizeof(std::uint32_t);

        entries.add(entry);
        words.add(isHalf ? std::uint32_t {constant.asHalfBits()} : constant.bits);
    }

    Vector<VkSpecializationMapEntry> entries;
    Vector<std::uint32_t> words;
    VkSpecializationInfo info {};
};
} // namespace eacp::GPU
