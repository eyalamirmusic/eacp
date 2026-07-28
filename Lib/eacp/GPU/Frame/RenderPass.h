#pragma once

#include "../Buffer/Buffer.h"
#include "../Texture/Texture.h"

#include <eacp/Core/Utils/Containers.h>

namespace eacp::GPU
{
class RenderPipeline;

// Records draw commands for a single render pass (MTLRenderCommandEncoder on
// Metal). Ends the encoder automatically on destruction. Obtained from
// Frame::beginPass.
class RenderPass
{
public:
    // Something holding drawing back that the pass has to collect before it
    // closes.
    //
    // A batching renderer does not draw when it is told to; it queues, so that
    // quads sharing a texture can go out as one draw. That leaves a queue only
    // it knows about, and the encoder closing is the deadline for it. Making
    // that the pass's business rather than the caller's is the difference
    // between an app forgetting a flush call and there being no call to forget:
    // the alternative fails by drawing nothing at all, silently, which is the
    // worst way for a renderer to fail.
    //
    // A participant must outlive the pass it joins - which is already the rule
    // for anything drawing into one, since its pipelines and buffers have to
    // survive until the command list is submitted.
    struct Participant
    {
        virtual ~Participant() = default;

        // Draw whatever is still queued. Called once, as the pass ends, and
        // before the encoder closes - so drawing from here is still legal.
        virtual void flushInto(RenderPass& pass) = 0;
    };

    // Joins this pass, to be flushed when it ends. Leaving is only needed by a
    // participant that stops drawing before the pass is over; one that simply
    // outlives it has already been dropped by then.
    void addParticipant(Participant& participant);
    void removeParticipant(Participant& participant);

    // targetWidth/targetHeight are the render target's size in *pixels*. The
    // pass needs them to clamp scissor rects: both backends reject a scissor
    // that leaves the render target (Metal API validation aborts), so a caller
    // scrolling a region partly off-screen would otherwise have to clamp by
    // hand at every call site.
    explicit RenderPass(void* encoder, int targetWidth = 0, int targetHeight = 0);
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    // Restricts rasterization to rect, in render-target *pixels* with the origin
    // at the top-left - the same orientation Metal's MTLScissorRect and D3D12's
    // D3D12_RECT use, and the same y-down sense as Graphics::Rect. Callers
    // working in logical points multiply by GPUView::backingScale() first.
    //
    // The rect is clamped to the render target, so a partly off-screen region
    // clips correctly instead of aborting. An empty or fully off-screen rect
    // discards every subsequent fragment, which is the useful behaviour for a
    // scrolled-away pane.
    //
    // Scissor state persists for the rest of the pass; call clearScissorRect to
    // go back to the full target. Nesting is the caller's job - the GPU has one
    // scissor rect, so a widget tree intersects rects on the way down.
    void setScissorRect(const Graphics::Rect& rect);

    // Restores rasterization to the whole render target.
    void clearScissorRect();

    void setPipeline(const RenderPipeline& pipeline);
    void setVertexBuffer(const Buffer& buffer, int index = 0);

    // Binds a texture to the fragment stage, sampled the way `sampling` says.
    // slot maps to Metal texture(slot) and to D3D t<slot>.
    //
    // The sampling comes from the caller rather than the texture because on
    // D3D12 it is baked into the shader (see TextureSampling), so it must be
    // the same value the shader was compiled with or the two backends draw
    // differently. Callers using the codegen layer get this for free —
    // ShaderProgram::bindTextures passes each member's declared sampling — and
    // only a hand-rolled bind has to supply it.
    void setFragmentTexture(const Texture& texture,
                            int slot = 0,
                            TextureSampling sampling = {});

    // Binds a Storage buffer for indexed reads in a shader stage - the thing a
    // vertex attribute stream is not. setVertexBuffer feeds the input assembler,
    // one element per vertex or per instance; this binds the whole buffer so the
    // shader can subscript it at an index it computed, which is what reading a
    // record a kernel produced by an id the shader worked out needs.
    //
    // slot maps to Metal buffer(bufferBase + slot) and to D3D
    // t(bufferRegisterBase + slot), above the texture registers the same way a
    // kernel's textures sit above its buffers. Read-only in both stages: a
    // render stage has no UAV here, so writing stays the compute path's job.
    void setVertexStorageBuffer(const Buffer& buffer, int slot = 0);
    void setFragmentStorageBuffer(const Buffer& buffer, int slot = 0);

    // Uploads small per-draw constant data to the vertex stage without a buffer
    // object (Metal setVertexBytes; a transient constant buffer on D3D12). slot
    // is the
    // uniform-block slot the generated shader declares (slot 0 = the first
    // uniform block). Ideal for values that change every frame, e.g. a transform.
    void setVertexBytes(const void* data, std::size_t bytes, int slot = 0);

    // The fragment-stage sibling of setVertexBytes, with the same slot mapping,
    // so one uniform block can be bound to both stages.
    void setFragmentBytes(const void* data, std::size_t bytes, int slot = 0);

    template <typename T>
    void setVertexUniform(const T& value, int slot = 0)
    {
        setVertexBytes(&value, sizeof(T), slot);
    }

    template <typename T>
    void setFragmentUniform(const T& value, int slot = 0)
    {
        setFragmentBytes(&value, sizeof(T), slot);
    }

    // Uploads a ShaderProgram's uniform block in one call: packs the program's
    // current member values and binds them. Templated so this header stays
    // independent of the codegen layer; any type exposing packedUniforms() and
    // uniformByteSize() works.
    template <typename Program>
    void setVertexUniforms(Program& program, int slot = 0)
    {
        setVertexBytes(program.packedUniforms(), program.uniformByteSize(), slot);
    }

    template <typename Program>
    void setFragmentUniforms(Program& program, int slot = 0)
    {
        setFragmentBytes(program.packedUniforms(), program.uniformByteSize(), slot);
    }

    void draw(int vertexCount, int firstVertex = 0);

    // Instanced sibling of draw: runs the vertex shader vertexCount times per
    // instance, for instanceCount instances. Per-vertex buffers (slots with
    // StepRate::PerVertex) rewind each instance; per-instance buffers
    // (StepRate::PerInstance) advance once per instance. firstInstance is a
    // constant added to the shader's instance-id lookup, useful for drawing a
    // subrange of a shared instance buffer.
    void drawInstanced(int vertexCount,
                       int instanceCount,
                       int firstVertex = 0,
                       int firstInstance = 0);

    // Draws indexCount indices from an Index-usage buffer, assembling with the
    // pipeline's topology. firstIndex is an offset into the index buffer.
    void drawIndexed(const Buffer& indices,
                     int indexCount,
                     IndexFormat format = IndexFormat::UInt32,
                     int firstIndex = 0);

    // Instanced sibling of drawIndexed: reuses the index buffer per instance.
    // Same step-rate semantics as drawInstanced.
    void drawIndexedInstanced(const Buffer& indices,
                              int indexCount,
                              int instanceCount,
                              IndexFormat format = IndexFormat::UInt32,
                              int firstIndex = 0,
                              int firstInstance = 0);

    // Binds a program's uniform block to the stage that reads it, and to no
    // other. The program answers per stage from the same walk that decided
    // whether to declare the block in that stage's generated function, so the
    // bind cannot disagree with the signature it is aimed at - and a stage that
    // never declared it is not bound at all, which is what the validation layer
    // reports as an unused binding. A program whose uniforms neither stage
    // reads binds nothing. draw(program) calls this; app code hand-rolling a
    // draw over its own geometry should call it rather than the two setters.
    template <typename Program>
    void setUniforms(Program& program, int slot = 0)
    {
        if (program.vertexReadsUniforms())
            setVertexUniforms(program, slot);

        if (program.fragmentReadsUniforms())
            setFragmentUniforms(program, slot);
    }

    // Binds and draws a prepared ShaderProgram in one call: its pipeline, vertex
    // buffer, uniform block and textures, then an indexed draw when the program
    // owns indices and a plain one otherwise. Templated so this header stays
    // independent of the codegen layer.
    template <typename Program>
    void draw(Program& program)
    {
        setPipeline(program.pipeline());
        setVertexBuffer(program.vertices());
        setUniforms(program);

        program.bindTextures(*this);
        program.bindBuffers(*this);

        if (program.hasIndices())
            drawIndexed(
                program.indices(), program.indexCount(), program.indexFormat());
        else
            draw(program.vertexCount());
    }

    // Instanced sibling of draw(program): binds the program's pipeline, its
    // per-vertex buffer (slot 0), every per-instance buffer, the uniform block
    // and textures, then issues an instanced draw - indexed when the program
    // owns indices, otherwise a plain instanced draw. firstInstance offsets into
    // the per-instance buffers, for drawing a subrange of a shared instance set
    // (e.g. one row at a time). Templated so this header stays independent of
    // the codegen layer.
    template <typename Program>
    void drawInstanced(Program& program, int instanceCount, int firstInstance = 0)
    {
        setPipeline(program.pipeline());
        setVertexBuffer(program.vertices(), 0);
        program.bindInstances(*this);
        setUniforms(program);

        program.bindTextures(*this);
        program.bindBuffers(*this);

        if (program.hasIndices())
            drawIndexedInstanced(program.indices(),
                                 program.indexCount(),
                                 instanceCount,
                                 program.indexFormat(),
                                 0,
                                 firstInstance);
        else
            drawInstanced(program.vertexCount(), instanceCount, 0, firstInstance);
    }

    void end();

    // The Metal buffer index the first uniform block binds to. Vertex buffers
    // take the low indices, so uniforms start above them - matching how
    // ComputePass reserves buffer(uniformBase) for its own uniforms. Reserving
    // a high slot lets multi-slot vertex layouts (e.g. instancing with a
    // per-vertex slot + N per-instance slots) coexist with uniforms without
    // the two paths clobbering each other's buffer(N).
    static constexpr int uniformBase = 16;

    // The Metal buffer index the first storage buffer binds to, above the
    // uniform blocks for the same reason those sit above the vertex buffers:
    // three kinds of binding share one index space, so each gets a range wide
    // enough that no layout can push one into the next.
    static constexpr int bufferBase = 24;

    // The D3D shader register a storage buffer takes. Textures hold t0.. on the
    // render signature, so buffers start above every texture slot - the mirror
    // of ComputePass::textureRegisterBase, where a kernel's buffers hold the low
    // registers and its textures start above them. D3D12Types.h holds the
    // emitter and the root signature to this number.
    static constexpr int bufferRegisterBase = 4;

private:
    // Flushes every participant, once. Called by end() on both backends before
    // the encoder closes, so a participant's draws still land on this pass.
    void drainParticipants();

    // Held here rather than in Native so both backends inherit one
    // implementation of this, and neither can drift from the other on when a
    // participant gets its last chance to draw.
    Vector<Participant*> participants;
    bool drained = false;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
