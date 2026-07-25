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

// The dialect this build's device compiles. Fixed at build time by which
// backend was selected, so it is defined alongside the codegen that picks the
// same one (Codegen/ShaderBuilder-*.cpp) rather than inferred from the platform:
// an Apple host builds either Metal or Vulkan. Hand-written native shader
// sources - the generated ones need no help - ask this before choosing which
// spelling to hand makeShaderLibrary.
ShaderBackend nativeShaderBackend();

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

    // Vulkan consumes SPIR-V and nothing else -- there is no in-driver compiler
    // for a text dialect the way Metal takes MSL and D3D takes HLSL -- so the
    // Vulkan backend's sources are words, not characters. Both stages live in
    // one module, entry points named as usual.
    static ShaderSource spirv(Vector<std::uint32_t> wordsToUse)
    {
        auto result = ShaderSource {};
        result.backend = ShaderBackend::Vulkan;
        result.words = std::move(wordsToUse);
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
    Vector<std::uint32_t> words; // SPIR-V; empty unless backend is Vulkan
    std::string vertexEntry = "vertexMain";
    std::string fragmentEntry = "fragmentMain";
    std::string computeEntry; // empty unless this is a compute source
    Vector<ResourceBinding> bindings;
};
} // namespace eacp::GPU
