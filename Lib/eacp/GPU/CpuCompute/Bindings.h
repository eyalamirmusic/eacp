#pragma once

#include <eacp/GPU/Codegen/ShaderValue.h>
#include <eacp/GPU/Frame/ComputePass.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>

// What a CPU dispatch reads and writes: one span per storage slot, set through
// the kernel's own buffer member so the slot comes from its handle. A
// Uniform<InputBuffer> is an InputBuffer, and so is what
// ShaderBuilder::inputBuffer() hands back, so the same overloads serve a
// kernel's members and a bare graph's handles. The spans are the caller's
// memory and must outlive the dispatch.

namespace eacp::GPU::CpuCompute
{
class Bindings
{
public:
    static constexpr int maxSlots = ComputePass::maxBufferSlots;

    struct Slot
    {
        const void* data = nullptr;
        std::uint32_t count = 0;
        ValueType element = ValueType::Float;
        BufferAccess access = BufferAccess::Read;
        bool bound = false;
    };

    bool set(const InputBuffer& member, std::span<const float> elements)
    {
        return bind(member.slot,
                    elements.data(),
                    elements.size(),
                    ValueType::Float,
                    BufferAccess::Read);
    }

    bool set(const OutputBuffer& member, std::span<float> elements)
    {
        return bind(member.slot,
                    elements.data(),
                    elements.size(),
                    ValueType::Float,
                    BufferAccess::Write);
    }

    bool set(const UIntInputBuffer& member, std::span<const std::uint32_t> elements)
    {
        return bind(member.slot,
                    elements.data(),
                    elements.size(),
                    ValueType::UInt,
                    BufferAccess::Read);
    }

    bool set(const UIntOutputBuffer& member, std::span<std::uint32_t> elements)
    {
        return bind(member.slot,
                    elements.data(),
                    elements.size(),
                    ValueType::UInt,
                    BufferAccess::Write);
    }

    bool set(const AtomicBuffer& member, std::span<std::uint32_t> elements)
    {
        return bind(member.slot,
                    elements.data(),
                    elements.size(),
                    ValueType::UInt,
                    BufferAccess::Atomic);
    }

    void clear(int slot)
    {
        if (isSlot(slot))
            slots[static_cast<std::size_t>(slot)] = {};
    }

    void clear() { slots.fill({}); }

    const Slot& slot(int index) const
    {
        static constexpr auto unbound = Slot {};
        return isSlot(index) ? slots[static_cast<std::size_t>(index)] : unbound;
    }

private:
    static bool isSlot(int slot) { return slot >= 0 && slot < maxSlots; }

    bool bind(int slot,
              const void* data,
              std::size_t count,
              ValueType element,
              BufferAccess access)
    {
        if (!isSlot(slot))
            return false;

        auto limit =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

        auto& entry = slots[static_cast<std::size_t>(slot)];
        entry.data = data;
        entry.count = static_cast<std::uint32_t>(count < limit ? count : limit);
        entry.element = element;
        entry.access = access;
        entry.bound = true;
        return true;
    }

    std::array<Slot, maxSlots> slots {};
};
} // namespace eacp::GPU::CpuCompute
