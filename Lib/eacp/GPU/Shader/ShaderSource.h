#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <string>

namespace eacp::GPU
{
enum class ShaderBackend
{
    Metal,
    DirectX
};

enum class ShaderStage
{
    Vertex,
    Fragment,
    Compute
};

enum class ResourceKind
{
    Buffer,
    Texture,
    Sampler
};

// An explicit shader resource binding. Kept as plain data (never inferred via
// runtime reflection) so a future C++ shader EDSL can populate the exact same
// description it generated the source for.
struct ResourceBinding
{
    ResourceKind kind = ResourceKind::Buffer;
    ShaderStage stage = ShaderStage::Vertex;
    int index = 0;
    std::string name;
};

// Native shader source plus the metadata a pipeline needs. The whole GPU layer
// downstream of this type consumes only this struct, so the planned shader EDSL
// becomes "a factory that returns a ShaderSource" with no call-site changes.
struct ShaderSource
{
    static ShaderSource msl(std::string sourceToUse)
    {
        auto result = ShaderSource {};
        result.backend = ShaderBackend::Metal;
        result.source = std::move(sourceToUse);
        return result;
    }

    static ShaderSource hlsl(std::string sourceToUse)
    {
        auto result = ShaderSource {};
        result.backend = ShaderBackend::DirectX;
        result.source = std::move(sourceToUse);
        return result;
    }

    ShaderSource& withVertex(std::string entry)
    {
        vertexEntry = std::move(entry);
        return *this;
    }

    ShaderSource& withFragment(std::string entry)
    {
        fragmentEntry = std::move(entry);
        return *this;
    }

    // Names the kernel entry point and marks this as a compute source: a library
    // built from it compiles only the compute stage, and ComputePipeline pulls
    // this function. Leave unset for a vertex/fragment source.
    ShaderSource& withCompute(std::string entry)
    {
        computeEntry = std::move(entry);
        return *this;
    }

    bool isCompute() const { return !computeEntry.empty(); }

    ShaderSource& withBinding(ResourceBinding binding)
    {
        bindings.add(std::move(binding));
        return *this;
    }

    ShaderBackend backend = ShaderBackend::Metal;
    std::string source;
    std::string vertexEntry = "vertexMain";
    std::string fragmentEntry = "fragmentMain";
    std::string computeEntry; // empty unless this is a compute source
    Vector<ResourceBinding> bindings;
};
} // namespace eacp::GPU
