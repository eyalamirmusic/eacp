#pragma once

#include <eacp/Core/Utils/Common.h>

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Shader/ShaderLibrary.h"
#include "../Shader/ShaderSource.h"

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

    Buffer makeBuffer(const void* data, std::size_t bytes)
    {
        return {*this, data, bytes};
    }

    template <typename T, std::size_t N>
    Buffer makeBuffer(const T (&array)[N])
    {
        return makeBuffer(array, sizeof(array));
    }

    ShaderLibrary makeShaderLibrary(const ShaderSource& source)
    {
        return {*this, source};
    }

    RenderPipeline makeRenderPipeline(const RenderPipelineDescriptor& descriptor)
    {
        return {*this, descriptor};
    }

    bool isValid() const;

    // Opaque native handles for cross-translation-unit use by other GPU types.
    void* nativeDevice() const;
    void* nativeQueue() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
