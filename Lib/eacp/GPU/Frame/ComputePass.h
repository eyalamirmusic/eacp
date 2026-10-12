#pragma once

#include "../Common.h"

#include "../Buffer/Buffer.h"
#include "../Shader/ShaderSource.h"
#include "../Texture/Texture.h"

namespace eacp::GPU
{
class ComputePipeline;
class Sampler;

// What an indirect dispatch reads out of a buffer: three threadgroup counts.
// Both backends take exactly this, in this order and at this size - Metal's
// MTLDispatchThreadgroupsIndirectArguments and D3D12's D3D12_DISPATCH_ARGUMENTS
// are the same three 32-bit unsigned integers - so a kernel writing one is
// writing the same three numbers whichever machine it runs on.
//
// **Threadgroups, not threads.** A kernel that has counted 1000 items divides
// by the group width the consuming kernel was compiled for - its own
// ComputeProgram::groupShape().x - and writes that, not 1000.
struct DispatchArguments
{
    std::uint32_t groupsX = 1;
    std::uint32_t groupsY = 1;
    std::uint32_t groupsZ = 1;
};

// Whether a pass's dispatches are ordered against each other. Serial is what
// every pass is unless it asked otherwise: each dispatch sees the writes of
// every dispatch recorded before it. Concurrent lets them overlap, and
// ComputePass::barrier() is what orders one stage against the next.
enum class DispatchOrder
{
    Serial,
    Concurrent
};

// Records dispatch commands for a single compute pass (MTLComputeCommandEncoder
// on Metal). Ends the encoder automatically on destruction. Obtained from
// CommandBuffer::beginCompute.
//
// Binding model: Metal uses one flat buffer-index space, D3D uses separate
// SRV/UAV/CBV register spaces. setInputBuffer/setOutputBuffer take a slot
// that maps to
// Metal buffer(slot) and to a D3D SRV t<slot> / UAV u<slot>; because Metal
// shares the space, an input and an output must use distinct slots. setBytes
// uploads a uniform block at Metal buffer(uniformBase + slot) and D3D CBV
// b<slot>, mirroring the render pass's hidden offset.
class ComputePass
{
public:
    explicit ComputePass(void* encoder, DispatchOrder order = DispatchOrder::Serial);
    ~ComputePass();

    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;

    // What this pass was asked to record so far, counted call by call: every
    // setPipeline, every dispatch (1D, 2D and 3D, the program forms included) and
    // every indirect one, and every barrier(). It is what was asked for, not what
    // the backend emitted - a barrier() in a Serial pass, which records nothing,
    // still counts, and so does a dispatch the pass dropped for having no
    // pipeline - so a test can pin a chain's structure on any backend.
    struct Recorded
    {
        bool operator==(const Recorded&) const = default;

        int pipelineSets = 0;
        int dispatches = 0;
        int indirectDispatches = 0;
        int barriers = 0;
    };

    const Recorded& recorded() const;

    // Binds the pipeline and adopts the threadgroup it was compiled for, which
    // is what every dispatch below is then encoded with.
    void setPipeline(const ComputePipeline& pipeline);

    // A read-only input (Metal device buffer / D3D shader-resource view) and a
    // read-write output (Metal device buffer / D3D unordered-access view).
    void setInputBuffer(const Buffer& buffer, int slot);
    void setOutputBuffer(const Buffer& buffer, int slot);

    // The same, bound from range.offset bytes in. The offset must be a multiple
    // of Device::storageBufferOffsetAlignment() - four on Metal and D3D12, the
    // device's own limit on Vulkan - and an offset off that grid binds nothing,
    // as one at or past the buffer's end does. range.bytes is not enforced. An
    // invalid range binds nothing either.
    void setInputBuffer(const BufferRange& range, int slot);
    void setOutputBuffer(const BufferRange& range, int slot);

    // The texture siblings, on a slot space of their own: a texture the kernel
    // samples or fetches, and one it writes. sampling is the configuration the
    // shader declared, exactly as in RenderPass::setFragmentTexture.
    //
    // An output texture must have been created with
    // TextureDescriptor::computeWrite; one that was not is dropped rather than
    // bound, since the resource has no view to bind through.
    void setInputTexture(const Texture& texture,
                         int slot,
                         TextureSampling sampling = {});

    // The per-bind sampler form, as RenderPass::setFragmentTexture describes:
    // MSL `sampler [[sampler(slot)]]`, HLSL
    // `register(s<RenderPass::samplerRegisterBase + slot>)`, Vulkan the combined
    // sampler at the texture's binding.
    void setInputTexture(const Texture& texture, int slot, const Sampler& sampler);
    void setOutputTexture(const Texture& texture, int slot);

    // One mip level of a texture, every slice of it, bound as the shader
    // declares the kind - texture2d, texture2d_array, texture3d and so on, a
    // volume with that level's images - so level 0 of the view is `level` of
    // the texture. What a hi-Z or bloom downscale is built from: a kernel
    // reading level N - 1 through setInputTextureLevel and writing level N
    // through setOutputTextureLevel of the same texture, one dispatch per
    // level.
    //
    // A level the texture lacks binds nothing, as does an output on a texture
    // that is not computeWrite. On D3D12 a read and a write of one texture put
    // one resource in two states, which its whole-resource tracking cannot
    // express: there the lower level is read through a second
    // setOutputTextureLevel and an RWTexture2D load. Metal and Vulkan take
    // either.
    void setInputTextureLevel(const Texture& texture,
                              int slot,
                              int level,
                              TextureSampling sampling = {});
    void setOutputTextureLevel(const Texture& texture, int slot, int level);

    // A cube or a cube array as the plain 2D array of slices it is stored as -
    // six faces, or layer * 6 + face - at one level, for a kernel that declared
    // texture2d_array (Texture2DArray / RWTexture2DArray, sampler2DArray /
    // image2DArray). That is how a kernel writes a cube at all, there being no
    // writable cube type in any of the three languages, and how bgfx's
    // IMAGE2D_ARRAY reaches one; the draw that samples the result binds the
    // cube as a cube. A TwoDArray binds as setInputTextureLevel and
    // setOutputTextureLevel would; a TwoD or ThreeD texture, or a level the
    // texture lacks, binds nothing, as does an output that is not
    // computeWrite.
    void setInputTextureAsArray(const Texture& texture,
                                int slot,
                                int level = 0,
                                TextureSampling sampling = {});
    void setOutputTextureAsArray(const Texture& texture, int slot, int level = 0);

    // The *depth buffer* of a render target, for a kernel to read: the compute
    // sibling of RenderPass::setFragmentDepthTexture, on this pass's input
    // texture slots and under the same rules. `renderTarget` is the colour
    // texture the buffer belongs to, created with
    // TextureDescriptor::sampleableDepth (one that was not binds nothing), and
    // the pass that drew it has ended with DepthAction::Keep. The kernel
    // declares the target's kind as a depth texture - depth2d,
    // depth2d_array, depthcube in MSL, Texture2D<float> or Texture2DArray<float>
    // in HLSL, sampler2D in GLSL - and reads it with read() or a sample, one
    // float. Level 0 of every slice, as the fragment bind gives. What a hi-Z
    // pyramid copies its first level out of, and what an ambient-occlusion
    // pass starts from.
    void setInputDepthTexture(const Texture& renderTarget,
                              int slot,
                              TextureSampling sampling = {});
    void setInputDepthTexture(const Texture& renderTarget,
                              int slot,
                              const Sampler& sampler);

    // One slice of that depth plane as a plain 2D depth texture, for a kernel
    // that declares depth2d: the compute twin of
    // RenderPass::setFragmentDepthTextureSlice, under its rules.
    void setInputDepthTextureSlice(const Texture& renderTarget,
                                   int slice,
                                   int slot,
                                   TextureSampling sampling = {});
    void setInputDepthTextureSlice(const Texture& renderTarget,
                                   int slice,
                                   int slot,
                                   const Sampler& sampler);

    // Uploads a small uniform block without a buffer object, like the render
    // pass's setVertexBytes. slot is the uniform-block slot (0 = first block).
    void setBytes(const void* data, std::int64_t bytes, int slot = 0);

    template <typename T>
    void setUniform(const T& value, int slot = 0)
    {
        setBytes(&value, (std::int64_t) sizeof(T), slot);
    }

    // Runs the kernel over count work items, in the bound pipeline's groups -
    // threadGroupWidth wide where it named no shape of its own.
    void dispatch(int count);

    // The 2D sibling, over a width × height grid. What anything image-shaped is
    // dispatched with, and what a kernel authored against threadPosition()
    // needs; the stock group is threadGroupSize2D squared.
    void dispatch(int width, int height);

    // The 3D sibling, over a width × height × depth volume. What a kernel
    // authored against threadPosition3() needs; the stock group is
    // threadGroupSize3D cubed.
    void dispatch(int width, int height, int depth);

    // Runs the kernel over a grid the **GPU** decided: the threadgroup counts
    // come from DispatchArguments living in a buffer an earlier kernel on this
    // command buffer wrote, and the CPU never learns the number. That is the
    // whole point - a stage whose size depends on what the stage before it found
    // would otherwise need a readback, and a readback is a round trip through
    // the host between two passes that were going to be adjacent.
    //
    // offsetInBytes must be a multiple of four and leave a whole
    // DispatchArguments in the buffer; an offset that does not dispatches
    // nothing.
    void dispatchIndirect(const Buffer& arguments, std::int64_t offsetInBytes = 0);

    // Binds and dispatches a prepared ComputeProgram in one call: its pipeline,
    // storage buffers and uniform block (including the implicit element count
    // its generated bounds guard reads), then a dispatch over count work items.
    // Templated so this header stays independent of the codegen layer.
    template <typename Program>
    void dispatch(Program& program, int count)
    {
        setPipeline(program.pipeline());
        program.bindResources(*this);

        // Sequenced separately: packing must happen before the size is read,
        // and argument evaluation order would not guarantee that.
        const auto* uniforms = program.packedUniforms(count);
        setBytes(uniforms, program.uniformByteSize());
        dispatch(count);
    }

    // The 2D form: the same binding, with the grid extents its guard reads in
    // place of the element count.
    template <typename Program>
    void dispatch(Program& program, int width, int height)
    {
        setPipeline(program.pipeline());
        program.bindResources(*this);

        const auto* uniforms = program.packedUniforms(width, height);
        setBytes(uniforms, program.uniformByteSize());
        dispatch(width, height);
    }

    // And the 3D one, over three extents.
    template <typename Program>
    void dispatch(Program& program, int width, int height, int depth)
    {
        setPipeline(program.pipeline());
        program.bindResources(*this);

        const auto* uniforms = program.packedUniforms(width, height, depth);
        setBytes(uniforms, program.uniformByteSize());
        dispatch(width, height, depth);
    }

    // The indirect form of the program dispatch: same binding, and a grid that
    // is not known here.
    //
    // guardCount is what the generated bounds guard compares against, and it
    // cannot be the real count - nothing on this side of the wire knows it. Pass
    // the **capacity**: the largest the count could be, which is usually the
    // size of the buffer the kernel writes. The guard then stops nothing short,
    // and a kernel that must not run past the real count reads it from a buffer
    // and returns itself. Both guards matter and neither replaces the other -
    // this one keeps threads inside the allocation, the kernel's own keeps them
    // inside the data.
    //
    // 1D only. A 2D or 3D indirect dispatch would take its extents beside an
    // offset and could not be told apart from this one, and nothing has needed
    // it; bind by hand and use the raw form above if it ever does.
    template <typename Program>
    void dispatchIndirect(Program& program,
                          const Buffer& arguments,
                          int guardCount,
                          std::int64_t offsetInBytes = 0)
    {
        setPipeline(program.pipeline());
        program.bindResources(*this);

        const auto* uniforms = program.packedUniforms(guardCount);
        setBytes(uniforms, program.uniformByteSize());
        dispatchIndirect(arguments, offsetInBytes);
    }

    // Every dispatch recorded before this one completes - its buffer and texture
    // writes visible - before any dispatch recorded after it begins. A no-op in
    // a Serial pass, where the dispatches are already ordered.
    void barrier();

    void end();

    // The Metal buffer index the first uniform block binds to. Storage buffers
    // take the low indices, so uniforms start above them.
    static constexpr int uniformBase = 16;

    // The stock threadgroup width of a 1D dispatch, used by every kernel that
    // named no ThreadGroupShape of its own.
    static constexpr int threadGroupWidth = 64;

    // The stock 2D group is this squared, which is the same 64 threads the 1D
    // path already budgets for - and square, so a group covers a tile rather
    // than a strip, which is what a kernel reading its neighbours wants.
    static constexpr int threadGroupSize2D = 8;

    // The stock 3D group is this cubed, which is those 64 threads again, as a
    // block rather than a tile.
    static constexpr int threadGroupSize3D = 4;

    // How many storage buffers a kernel may bind on every backend. The D3D12
    // compute root signature declares a root SRV and a root UAV per slot below
    // this and spends the rest of its 64 DWORDs on textures and samplers, so a
    // slot past it binds nowhere there - and a kernel reading an unbound buffer
    // reads zeroes rather than failing, which is silent everywhere but in the
    // picture. So the emitter asserts on a kernel that asks for more, rather
    // than letting the last one fall off the end, and the CPU interpreter's
    // tables are this wide.
    static constexpr int portableBufferSlots = 8;

    // The most storage buffers any backend binds: Metal's buffer indices are one
    // flat space with the uniform block at uniformBase on top, and Vulkan
    // declares slots past portableBufferSlots above that block. A hand-written
    // kernel may use slots up to Device::maxComputeBufferSlots(), which is this
    // on Metal, this or the device's own storage-buffer limit on Vulkan, and
    // portableBufferSlots on D3D12. A bind at a slot the device does not have is
    // dropped.
    static constexpr int maxBufferSlots = uniformBase;

    // The D3D shader register a kernel's first texture takes. A texture shares
    // the t/u register spaces with the storage buffers there, and the two slot
    // spaces are counted separately, so textures start above every buffer slot
    // D3D12 binds - the emitter writes these registers and the root signature
    // declares them, and D3D12Types.h holds the two to the same number. Metal is
    // unaffected: its texture indices are a space of their own.
    static constexpr int textureRegisterBase = portableBufferSlots;

private:
    // The backend half of each call above, one per platform file; the public
    // forms count and forward, so the counting is written once.
    void encodePipeline(const ComputePipeline& pipeline);
    void encodeDispatch(int count);
    void encodeDispatch(int width, int height);
    void encodeDispatch(int width, int height, int depth);
    void encodeDispatchIndirect(const Buffer& arguments, std::int64_t offsetInBytes);
    void encodeBarrier();

    // The group each dispatch is encoded with: the bound pipeline's own, or the
    // stock shape for the dispatch's rank when it carried none.
    ThreadGroupShape groupFor1D() const;
    ThreadGroupShape groupFor2D() const;
    ThreadGroupShape groupFor3D() const;

    ThreadGroupShape boundGroup;

    // Whether the last setPipeline had a pipeline to bind. A pipeline that
    // would not build is not something a dispatch can report - the encoder is
    // recorded against and the failure surfaces much later, as a crash on
    // Metal, where an encoder with no pipeline state aborts the process - so a
    // dispatch under one is dropped instead. Every dispatch below tests it.
    //
    // False until something is bound, which makes a pass that dispatches
    // before it binds a no-op rather than whatever the encoder held.
    bool boundPipeline = false;

    Recorded counts;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
