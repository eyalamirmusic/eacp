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

    // The render target's size in pixels - the units setScissorRect and
    // setViewport take, and what a caller dividing the target between several
    // viewports has to know to divide it.
    //
    // Worth having rather than deriving, because the obvious derivation is
    // wrong: a view's bounds times GPUView::backingScale() is the size of the
    // *drawable*, and a pass rendering a snapshot (View::renderToImage) or into
    // a texture is a different size entirely. A split-screen layout computed
    // the derived way lands outside a snapshot's target, where setViewport is
    // deliberately a no-op, and the panes silently collapse into one.
    //
    // Zero on a pass that was given no size.
    int targetWidth() const;
    int targetHeight() const;

    // Maps clip space onto rect instead of onto the whole render target, in
    // render-target *pixels* with the origin at the top-left - the same units
    // and orientation setScissorRect uses.
    //
    // This is not a scissor and does not clip: it moves and scales what is
    // drawn. Geometry filling clip space fills rect; geometry covering the left
    // half of clip space covers the left half of rect, wherever rect is. That
    // is what renders a scene into one pane of a split screen, or a shadow map
    // into one tile of an atlas, without touching a single vertex - and it is
    // why a scissor cannot do the job, since a scissor set to the same
    // rectangle would simply throw that geometry away.
    //
    // near/far remap the depth a fragment writes: a viewport of [0.5, 1] puts
    // everything it draws behind everything drawn at the default [0, 1],
    // whatever its own geometry says. Both backends take the same range and
    // mean the same thing by it.
    //
    // A rect that is empty, or not wholly inside the render target, is a no-op
    // - deliberately not clamped, for the reason Texture::update gives about
    // regions. Clamping a scissor is right because the clipped picture is the
    // one the caller wanted; clamping a *viewport* would keep drawing and
    // silently squash the image into the clamped rectangle, which looks like a
    // rendering bug anywhere but here. Nothing appearing is easier to find.
    //
    // Viewport state persists for the rest of the pass; call clearViewport to
    // go back to the whole target.
    void setViewport(const Graphics::Rect& rect,
                     float nearDepth = 0.f,
                     float farDepth = 1.f);

    // Restores the viewport to the whole render target at depth [0, 1].
    void clearViewport();

    void setPipeline(const RenderPipeline& pipeline);

    // The value every stencil comparison in this pass tests against, and the
    // value StencilOp::Replace writes. Pass state rather than pipeline state on
    // both APIs, and for the reason it is worth having: the algorithms that use
    // stencil draw the same geometry with the same pipeline against a different
    // reference, so making it part of the pipeline would compile a program per
    // value.
    //
    // Only the bits RenderPipelineDescriptor::stencilReadMask allows take part
    // in the comparison. Persists for the rest of the pass; a pass that never
    // calls this compares against zero.
    void setStencilReference(unsigned int value);

    void setVertexBuffer(const Buffer& buffer, int index = 0);

    // The same, reading from range.offset bytes into its buffer rather than
    // from the start: what a StreamingBuffers write hands back, and what lets
    // every draw of a frame bind the one arena they were all streamed into.
    // Vertex zero of the draw is the byte at the offset. An invalid range
    // binds nothing.
    void setVertexBuffer(const BufferRange& range, int index = 0);

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

    // Binds the *depth buffer* of a render target to the fragment stage, on the
    // same slot space and with the same sampling rule as the call above. The
    // shader's matching declaration is ShaderBuilder::depthTexture, whose
    // sample() gives back one float rather than four - which is what a depth
    // format is on both backends.
    //
    // `renderTarget` is the colour texture the buffer belongs to, not a texture
    // of its own: a depth attachment is created with its target, lives exactly
    // as long, and has no independent existence to hand out. It must have been
    // created with TextureDescriptor::sampleableDepth; one that was not is a
    // no-op here rather than a read of something undefined, and
    // Texture::hasSampleableDepth is what says which.
    //
    // **The pass rendering into that target cannot be this one.** A texture is
    // not sampleable by the pass that is writing it, and a depth attachment is
    // a texture; so the pass that drew the depth has to have ended, with
    // DepthAction::Keep so that what it wrote is still there. That is the same
    // shape a colour copy out of a render target already has, one plane along.
    void setFragmentDepthTexture(const Texture& renderTarget,
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
    //
    // baseVertex is added to every index before the vertex is fetched, which is
    // what lets one vertex buffer hold many meshes while each keeps indices
    // starting from zero. Without it a caller packing meshes together has to
    // bake the offset into the index values as it copies them in - a pass over
    // every index, and one that forces 32-bit indices as soon as the shared
    // buffer passes 65536 vertices even when no single mesh is near that.
    void drawIndexed(const Buffer& indices,
                     int indexCount,
                     IndexFormat format = IndexFormat::UInt32,
                     int firstIndex = 0,
                     int baseVertex = 0);

    // The same over a slice of an index buffer: index zero is the one at
    // range.offset, and firstIndex counts on from there. The indexed sibling
    // of the setVertexBuffer overload, for indices streamed into an arena
    // beside the vertices.
    void drawIndexed(const BufferRange& indices,
                     int indexCount,
                     IndexFormat format = IndexFormat::UInt32,
                     int firstIndex = 0,
                     int baseVertex = 0);

    // Instanced sibling of drawIndexed: reuses the index buffer per instance.
    // Same step-rate semantics as drawInstanced, and the same baseVertex.
    void drawIndexedInstanced(const Buffer& indices,
                              int indexCount,
                              int instanceCount,
                              IndexFormat format = IndexFormat::UInt32,
                              int firstIndex = 0,
                              int firstInstance = 0,
                              int baseVertex = 0);

    void drawIndexedInstanced(const BufferRange& indices,
                              int indexCount,
                              int instanceCount,
                              IndexFormat format = IndexFormat::UInt32,
                              int firstIndex = 0,
                              int firstInstance = 0,
                              int baseVertex = 0);

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

    // Everything a program needs bound before a draw - its pipeline, its uniform
    // block on the stage that reads it, its textures and its storage buffers -
    // with the geometry taken from `vertices` rather than from the program, and
    // no draw issued.
    //
    // This is draw(program) with two things taken off it, and both are what the
    // consumer it exists for cannot supply. Geometry the *app* owns - a buffer it
    // updates in place, packs itself, or streams - never becomes the program's,
    // and such a buffer is usually drawn as several sub-ranges differing in one
    // piece of state, so there is no single vertex count to hand over either.
    //
    // Per draw the caller then re-binds whatever it is actually changing -
    // program.bindTextures(*this) for a texture, setUniforms(program) for a
    // uniform - and calls draw(vertexCount, firstVertex). What it does not have
    // to do is restate the list above, which is the point: a program bound by
    // hand and a program drawn by draw(program) go through the same lines, so a
    // seventh thing added here reaches both.
    //
    // The program need not own a vertex buffer at all when this overload is
    // used. Nothing here asks it for one.
    template <typename Program>
    void bind(Program& program, const Buffer& vertices)
    {
        setPipeline(program.pipeline());
        setVertexBuffer(vertices);
        setUniforms(program);

        program.bindTextures(*this);
        program.bindBuffers(*this);
    }

    // The same over a slice of a buffer - geometry the app streamed rather than
    // geometry it keeps.
    template <typename Program>
    void bind(Program& program, const BufferRange& vertices)
    {
        setPipeline(program.pipeline());
        setVertexBuffer(vertices);
        setUniforms(program);

        program.bindTextures(*this);
        program.bindBuffers(*this);
    }

    // The same over the program's own geometry, for a caller drawing it as
    // sub-ranges rather than all at once.
    template <typename Program>
    void bind(Program& program)
    {
        bind(program, program.vertices());
    }

    // Binds and draws a prepared ShaderProgram in one call: its pipeline, vertex
    // buffer, uniform block and textures, then an indexed draw when the program
    // owns indices and a plain one otherwise. Templated so this header stays
    // independent of the codegen layer.
    template <typename Program>
    void draw(Program& program)
    {
        bind(program);

        if (program.hasIndices())
            drawIndexed(
                program.indices(), program.indexCount(), program.indexFormat());
        else
            draw(program.vertexCount());
    }

    // One sub-range of app-owned geometry, bound and drawn: draw(program) except
    // that the vertices and the count come from the caller. For a caller with a
    // single range - one with several should bind() once and loop, rather than
    // re-binding the pipeline and re-uploading the uniform block per draw.
    template <typename Program>
    void draw(Program& program,
              const Buffer& vertices,
              int vertexCount,
              int firstVertex = 0)
    {
        bind(program, vertices);
        draw(vertexCount, firstVertex);
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
        bind(program);
        program.bindInstances(*this);

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
