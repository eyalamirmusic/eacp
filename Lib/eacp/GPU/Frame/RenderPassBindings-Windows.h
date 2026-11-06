#pragma once

#include "IndirectDrawPlan.h"
#include "RenderPass.h"

#include "../Shader/ShaderSource.h"
#include "../Windows/D3D12Types.h"

namespace eacp::GPU
{
struct StageTextureSlot
{
    ShaderStage stage = ShaderStage::Fragment;
    int slot = 0;
};

struct IndexRange
{
    BufferRange indices;
    IndexFormat format = IndexFormat::UInt32;
};

struct IndirectDrawCount
{
    const Buffer* buffer = nullptr;
    std::int64_t offset = 0;
};

void bindStageTexture(D3D12Encoder* encoder,
                      StageTextureSlot target,
                      const Texture& texture);
void bindStageDepthTexture(D3D12Encoder* encoder,
                           StageTextureSlot target,
                           const Texture& renderTarget);
void bindStageDepthSlice(D3D12Encoder* encoder,
                         StageTextureSlot target,
                         const Texture& renderTarget,
                         int slice);
void bindPerBindSampler(D3D12Encoder* encoder, int slot, const Sampler& sampler);

// The address a stage's root SRV binds to, with the buffer moved into the state
// a shader read needs. A kernel that wrote this buffer left it in
// UNORDERED_ACCESS; the barrier here is what orders the draw behind that write.
// Zero when there is nothing to bind - no buffer, no resource, or an offset
// outside it - and the shader's element zero is the element at the offset.
D3D12_GPU_VIRTUAL_ADDRESS storageBufferAddress(CommandContext& commands,
                                               const BufferRange& range);

void bindVertexBufferRange(D3D12Encoder& encoder,
                           const BufferRange& range,
                           int index);

void bindConstantBytes(D3D12Encoder& encoder,
                       UINT rootParameter,
                       const void* data,
                       int bytes);

// The index buffer view for a slice, bound, with the resource moved into the
// state an index read needs. False when there is nothing to bind - no buffer,
// no resource, or an offset past the end of it - so the draw that follows can
// be skipped rather than issued over nothing. The view starts at the slice
// and runs to the buffer's end, the same shape the vertex bind takes, so
// firstIndex counts on from the slice's own index zero.
bool bindIndexRange(CommandContext& commands,
                    const BufferRange& indices,
                    IndexFormat format);

// One ExecuteIndirect for the whole run: the stride is the signature's, and
// the count buffer, where there is one, caps the run at the GPU's own number.
void executeIndirectDraws(CommandContext& commands,
                          const IndirectDrawPlan& plan,
                          const Buffer& arguments,
                          IndirectDrawCount count);
void executeIndexedIndirectDraws(CommandContext& commands,
                                 const IndirectDrawPlan& plan,
                                 const Buffer& arguments,
                                 IndirectDrawCount count);
} // namespace eacp::GPU
