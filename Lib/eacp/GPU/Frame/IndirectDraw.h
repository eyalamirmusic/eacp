#pragma once

#include <cstdint>

namespace eacp::GPU
{
// What RenderPass::drawIndirect reads out of a buffer for one draw: Metal's
// MTLDrawPrimitivesIndirectArguments, Vulkan's VkDrawIndirectCommand and
// D3D12's D3D12_DRAW_ARGUMENTS, which are the same four 32-bit integers in the
// same order, so a kernel writing one writes it for every backend.
struct DrawArguments
{
    std::uint32_t vertexCount = 0;
    std::uint32_t instanceCount = 1;
    std::uint32_t firstVertex = 0;
    std::uint32_t firstInstance = 0;
};

// The indexed sibling, for drawIndexedIndirect: MTLDrawIndexedPrimitives-
// IndirectArguments, VkDrawIndexedIndirectCommand and
// D3D12_DRAW_INDEXED_ARGUMENTS. firstIndex counts from the start of the bound
// index range, and baseVertex is signed, as in drawIndexed.
struct DrawIndexedArguments
{
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 1;
    std::uint32_t firstIndex = 0;
    std::int32_t baseVertex = 0;
    std::uint32_t firstInstance = 0;
};

static_assert(sizeof(DrawArguments) == 16);
static_assert(sizeof(DrawIndexedArguments) == 20);
} // namespace eacp::GPU
