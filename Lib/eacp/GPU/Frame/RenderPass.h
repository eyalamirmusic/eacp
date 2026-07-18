#pragma once

#include "../Buffer/Buffer.h"

namespace eacp::GPU
{
class RenderPipeline;
class Texture;

// Records draw commands for a single render pass (MTLRenderCommandEncoder on
// Metal). Ends the encoder automatically on destruction. Obtained from
// Frame::beginPass.
class RenderPass
{
public:
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

    // Binds a texture and its baked sampler to the fragment stage. slot maps to
    // Metal texture(slot)/sampler(slot) and to D3D t<slot>/s<slot>; the
    // generated shaders declare texture and sampler at the same index.
    void setFragmentTexture(const Texture& texture, int slot = 0);

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

    // Binds and draws a prepared ShaderProgram in one call: its pipeline, vertex
    // buffer, uniform block and textures, then an indexed draw when the program
    // owns indices and a plain one otherwise. The uniform block is bound to both
    // stages so a uniform works wherever define() reads it; a stage whose
    // generated function never declares the block ignores the bind. Templated so
    // this header stays independent of the codegen layer.
    template <typename Program>
    void draw(Program& program)
    {
        setPipeline(program.pipeline());
        setVertexBuffer(program.vertices());

        if (program.hasUniforms())
        {
            setVertexUniforms(program);
            setFragmentUniforms(program);
        }

        program.bindTextures(*this);

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

        if (program.hasUniforms())
        {
            setVertexUniforms(program);
            setFragmentUniforms(program);
        }

        program.bindTextures(*this);

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

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
