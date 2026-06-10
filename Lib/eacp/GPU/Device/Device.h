#pragma once

#include <eacp/Core/Utils/Common.h>

#include "../Buffer/Buffer.h"
#include "../CommandBuffer/CommandBuffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Shader/ShaderLibrary.h"
#include "../Shader/ShaderSource.h"
#include "../Texture/Texture.h"

#include <cstddef>

namespace eacp::GPU
{
// The GPU device (MTLDevice + command queue on Metal). Owns the resource
// factories. Most apps use the process-wide Device::shared().
class Device
{
public:
    Device();

    static Device& shared();

    Buffer makeBuffer(const void* data,
                      std::size_t bytes,
                      BufferUsage usage = BufferUsage::Vertex)
    {
        return {*this, data, bytes, usage};
    }

    template <typename T, std::size_t N>
    Buffer makeBuffer(const T (&array)[N], BufferUsage usage = BufferUsage::Vertex)
    {
        return makeBuffer(array, sizeof(array), usage);
    }

    // An uninitialised buffer of the given size, e.g. a compute output target.
    Buffer makeBuffer(std::size_t bytes, BufferUsage usage = BufferUsage::Storage)
    {
        return {*this, nullptr, bytes, usage};
    }

    // A 2D texture from tightly packed 4-byte pixels (row 0 at the top), or an
    // uninitialised texture when pixels is null.
    Texture makeTexture(const TextureDescriptor& descriptor,
                        const void* pixels = nullptr)
    {
        return {*this, descriptor, pixels};
    }

    ShaderLibrary makeShaderLibrary(const ShaderSource& source)
    {
        return {*this, source};
    }

    RenderPipeline makeRenderPipeline(const RenderPipelineDescriptor& descriptor)
    {
        return {*this, descriptor};
    }

    ComputePipeline makeComputePipeline(const ShaderLibrary& library)
    {
        return {*this, library};
    }

    CommandBuffer makeCommandBuffer() { return CommandBuffer {*this}; }

    bool isValid() const;

    // Opaque native handles for cross-translation-unit use by other GPU types.
    void* nativeDevice() const;
    void* nativeQueue() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
