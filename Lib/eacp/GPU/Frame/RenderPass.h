#pragma once

#include <eacp/Core/Utils/Common.h>

#include <cstddef>

namespace eacp::GPU
{
class RenderPipeline;
class Buffer;
class Texture;

// Records draw commands for a single render pass (MTLRenderCommandEncoder on
// Metal). Ends the encoder automatically on destruction. Obtained from
// Frame::beginPass.
class RenderPass
{
public:
    explicit RenderPass(void* encoder);
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    void setPipeline(const RenderPipeline& pipeline);
    void setVertexBuffer(const Buffer& buffer, int index = 0);

    // Binds a texture and its baked sampler to the fragment stage. slot maps to
    // Metal texture(slot)/sampler(slot) and to D3D t<slot>/s<slot>; the
    // generated shaders declare texture and sampler at the same index.
    void setFragmentTexture(const Texture& texture, int slot = 0);

    // Uploads small per-draw constant data to the vertex stage without a buffer
    // object (Metal setVertexBytes; a constant buffer on D3D11). slot is the
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

    // Binds and draws a prepared ShaderProgram in one call: its pipeline, vertex
    // buffer, uniform block and textures, then its vertex count. The block is
    // bound to both stages so a uniform works wherever define() reads it; a
    // stage whose generated function never declares the block ignores the bind.
    // Templated so this header stays independent of the codegen layer.
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
        draw(program.vertexCount());
    }

    void end();

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
