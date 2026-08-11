#pragma once

#include "../Buffer/Buffer.h"
#include "../Texture/Texture.h"

#include <eacp/Core/Utils/Containers.h>

namespace eacp::GPU
{
class RenderPipeline;

// Records draw commands for a single render pass, obtained from
// Frame::beginPass. Ends the encoder on destruction.
class RenderPass
{
public:
    // A batching renderer that queues its draws; must outlive the pass it joins.
    struct Participant
    {
        virtual ~Participant() = default;

        // Called once as the pass ends, before the encoder closes, so drawing
        // from here is still legal.
        virtual void flushInto(RenderPass& pass) = 0;
    };

    // Leaving is only needed by a participant that stops drawing early.
    void addParticipant(Participant& participant);
    void removeParticipant(Participant& participant);

    // targetWidth/targetHeight are in pixels; the pass clamps scissor rects to
    // them, since both backends reject a scissor leaving the render target.
    explicit RenderPass(void* encoder, int targetWidth = 0, int targetHeight = 0);
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    // rect is in render-target pixels, origin top-left, y-down. Clamped to the
    // target; an empty or off-screen rect discards every later fragment. State
    // persists for the pass, and nesting/intersection is the caller's job.
    void setScissorRect(const Graphics::Rect& rect);

    void clearScissorRect();

    // Maps clip space onto rect (same units as setScissorRect) rather than
    // clipping to it; near/far remap written depth. A rect that is empty or not
    // wholly inside the target is a no-op, clamping it squashing the image.
    void setViewport(const Graphics::Rect& rect,
                     float nearDepth = 0.f,
                     float farDepth = 1.f);

    void clearViewport();

    void setPipeline(const RenderPipeline& pipeline);
    void setVertexBuffer(const Buffer& buffer, int index = 0);

    // slot maps to Metal texture(slot) and D3D t<slot>. sampling must match what
    // the shader was compiled with, since D3D12 bakes it in (see
    // TextureSampling); ShaderProgram::bindTextures supplies it automatically.
    void setFragmentTexture(const Texture& texture,
                            int slot = 0,
                            TextureSampling sampling = {});

    // Whole-buffer binding for shader-computed indexing, as against
    // setVertexBuffer's per-vertex stream. Read-only in both stages; slot maps
    // to Metal buffer(bufferBase + slot) and D3D t(bufferRegisterBase + slot).
    void setVertexStorageBuffer(const Buffer& buffer, int slot = 0);
    void setFragmentStorageBuffer(const Buffer& buffer, int slot = 0);

    // Per-draw constants without a buffer object. slot is the uniform-block slot
    // the generated shader declares.
    void setVertexBytes(const void* data, std::size_t bytes, int slot = 0);
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

    // Any type exposing packedUniforms() and uniformByteSize() works, keeping
    // this header independent of the codegen layer.
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

    // StepRate::PerVertex slots rewind each instance; StepRate::PerInstance
    // slots advance once per instance. firstInstance offsets the instance-id.
    void drawInstanced(int vertexCount,
                       int instanceCount,
                       int firstVertex = 0,
                       int firstInstance = 0);

    // firstIndex offsets into the index buffer; baseVertex is added to every
    // index before the fetch, so meshes sharing one vertex buffer can each keep
    // indices starting at zero.
    void drawIndexed(const Buffer& indices,
                     int indexCount,
                     IndexFormat format = IndexFormat::UInt32,
                     int firstIndex = 0,
                     int baseVertex = 0);

    void drawIndexedInstanced(const Buffer& indices,
                              int indexCount,
                              int instanceCount,
                              IndexFormat format = IndexFormat::UInt32,
                              int firstIndex = 0,
                              int firstInstance = 0,
                              int baseVertex = 0);

    // Binds the uniform block only to the stages that declared it, so the
    // validation layer sees no unused binding. Prefer this to the two setters.
    template <typename Program>
    void setUniforms(Program& program, int slot = 0)
    {
        if (program.vertexReadsUniforms())
            setVertexUniforms(program, slot);

        if (program.fragmentReadsUniforms())
            setFragmentUniforms(program, slot);
    }

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

    // firstInstance offsets into the per-instance buffers, for drawing a
    // subrange of a shared instance set.
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

    // Metal buffer indices: vertex buffers, then uniforms, then storage buffers
    // share one index space, each range wide enough that no layout overruns it.
    static constexpr int uniformBase = 16;
    static constexpr int bufferBase = 24;

    // D3D register for the first storage buffer, above the texture slots at
    // t0... D3D12Types.h holds the emitter and root signature to this number.
    static constexpr int bufferRegisterBase = 4;

private:
    void drainParticipants();

    Vector<Participant*> participants;
    bool drained = false;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
