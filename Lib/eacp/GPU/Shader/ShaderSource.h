#pragma once

#include "../Common.h"

namespace eacp::GPU
{
enum class ShaderBackend
{
    Metal,
    DirectX,
    Vulkan
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

// The threadgroup a kernel is dispatched in, chosen by its author. A default
// shape means the stock one for the kernel's rank - 64 threads in 1D, 8x8 in
// 2D, 4x4x4 in 3D - which is what ComputePass::threadGroupWidth and its two
// siblings spell.
struct ThreadGroupShape
{
    bool isSet() const { return x > 0; }
    int threadCount() const { return x * y * z; }

    int x = 0;
    int y = 1;
    int z = 1;
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

    // Both stages share one string behind #ifdef EACP_VERTEX / EACP_FRAGMENT, so
    // the entry is always main; only computeEntry is still read, by isCompute().
    static ShaderSource glsl(std::string sourceToUse)
    {
        auto result = ShaderSource {};
        result.backend = ShaderBackend::Vulkan;
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

    // The group the kernel's entry point was emitted for, which is the group
    // the pass has to dispatch it in. Left unset for a hand-written source, and
    // the stock shape for the dispatch's rank is used.
    ShaderSource& withThreadGroup(ThreadGroupShape shape)
    {
        threadGroup = shape;
        return *this;
    }

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
    ThreadGroupShape threadGroup;
    Vector<ResourceBinding> bindings;
};
} // namespace eacp::GPU
