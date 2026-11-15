#pragma once

#include <eacp/GPU/Codegen/ShaderStruct.h>
#include <eacp/GPU/Frame/ComputePass.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

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
    static constexpr int maxSlots = ComputePass::portableBufferSlots;

    struct Slot
    {
        const void* data = nullptr;
        std::uint32_t count = 0;
        ValueType element = ValueType::Float;
        BufferAccess access = BufferAccess::Read;
        bool bound = false;
        int structStride = 0;
    };

    bool set(const InputBuffer& member, std::span<const float> elements);
    bool set(const OutputBuffer& member, std::span<float> elements);
    bool set(const UIntInputBuffer& member, std::span<const std::uint32_t> elements);
    bool set(const UIntOutputBuffer& member, std::span<std::uint32_t> elements);
    bool set(const IntInputBuffer& member, std::span<const std::int32_t> elements);
    bool set(const IntOutputBuffer& member, std::span<std::int32_t> elements);

    // The f16 buffers, from their IEEE binary16 bits: each element is two
    // bytes, widened where it is read and narrowed where it is written.
    bool set(const HalfInputBuffer& member, std::span<const std::uint16_t> elements);
    bool set(const HalfOutputBuffer& member, std::span<std::uint16_t> elements);
    bool set(const AtomicBuffer& member, std::span<std::uint32_t> elements);
    bool set(const IntAtomicBuffer& member, std::span<std::int32_t> elements);

    // A buffer of structs, from its bytes laid out as StructLayout<T> says
    // (StructBufferData's), or from host records whose size is the struct's
    // stride - a C++ mirror of it. The elements are the whole strides the
    // bytes hold.
    template <ShaderStructLike T>
    bool set(const StructInputBuffer<T>& member, std::span<const std::byte> bytes)
    {
        return bindStruct(member.slot,
                          bytes.data(),
                          bytes.size(),
                          StructLayout<T>::stride,
                          BufferAccess::Read);
    }

    template <ShaderStructLike T>
    bool set(const StructOutputBuffer<T>& member, std::span<std::byte> bytes)
    {
        return bindStruct(member.slot,
                          bytes.data(),
                          bytes.size(),
                          StructLayout<T>::stride,
                          BufferAccess::Write);
    }

    template <ShaderStructLike T, typename Record>
        requires(!std::same_as<std::remove_cv_t<Record>, std::byte>)
    bool set(const StructInputBuffer<T>& member, std::span<const Record> records)
    {
        static_assert(sizeof(Record) == StructLayout<T>::stride,
                      "eacp: a host record of a struct buffer is the struct's "
                      "stride in size");
        return set(member, std::as_bytes(records));
    }

    template <ShaderStructLike T, typename Record>
        requires(!std::same_as<std::remove_cv_t<Record>, std::byte>)
    bool set(const StructOutputBuffer<T>& member, std::span<Record> records)
    {
        static_assert(sizeof(Record) == StructLayout<T>::stride,
                      "eacp: a host record of a struct buffer is the struct's "
                      "stride in size");
        return set(member, std::as_writable_bytes(records));
    }

    void clear(int slot);
    void clear();

    const Slot& slot(int index) const;

private:
    static constexpr bool isSlot(int slot) { return slot >= 0 && slot < maxSlots; }

    bool bind(int slot,
              const void* data,
              std::size_t count,
              ValueType element,
              BufferAccess access);

    bool bindStruct(int slot,
                    const void* data,
                    std::size_t bytes,
                    int stride,
                    BufferAccess access);

    std::array<Slot, maxSlots> slots {};
};
} // namespace eacp::GPU::CpuCompute
