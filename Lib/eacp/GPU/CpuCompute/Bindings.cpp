#include "Bindings.h"

#include <limits>

namespace eacp::GPU::CpuCompute
{
bool Bindings::set(const InputBuffer& member, std::span<const float> elements)
{
    return bind(member.slot,
                elements.data(),
                elements.size(),
                ValueType::Float,
                BufferAccess::Read);
}

bool Bindings::set(const OutputBuffer& member, std::span<float> elements)
{
    return bind(member.slot,
                elements.data(),
                elements.size(),
                ValueType::Float,
                BufferAccess::Write);
}

bool Bindings::set(const UIntInputBuffer& member,
                   std::span<const std::uint32_t> elements)
{
    return bind(member.slot,
                elements.data(),
                elements.size(),
                ValueType::UInt,
                BufferAccess::Read);
}

bool Bindings::set(const UIntOutputBuffer& member, std::span<std::uint32_t> elements)
{
    return bind(member.slot,
                elements.data(),
                elements.size(),
                ValueType::UInt,
                BufferAccess::Write);
}

bool Bindings::set(const AtomicBuffer& member, std::span<std::uint32_t> elements)
{
    return bind(member.slot,
                elements.data(),
                elements.size(),
                ValueType::UInt,
                BufferAccess::Atomic);
}

void Bindings::clear(int slot)
{
    if (isSlot(slot))
        slots[static_cast<std::size_t>(slot)] = {};
}

void Bindings::clear()
{
    slots.fill({});
}

const Bindings::Slot& Bindings::slot(int index) const
{
    static constexpr auto unbound = Slot {};
    return isSlot(index) ? slots[static_cast<std::size_t>(index)] : unbound;
}

bool Bindings::bind(int slot,
                    const void* data,
                    std::size_t count,
                    ValueType element,
                    BufferAccess access)
{
    if (!isSlot(slot))
        return false;

    auto limit = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

    auto& entry = slots[static_cast<std::size_t>(slot)];
    entry.data = data;
    entry.count = static_cast<std::uint32_t>(count < limit ? count : limit);
    entry.element = element;
    entry.access = access;
    entry.bound = true;
    return true;
}
} // namespace eacp::GPU::CpuCompute
