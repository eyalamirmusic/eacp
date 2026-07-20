#pragma once

#include "../Texture/Texture.h"

namespace eacp::GPU
{
// How a shader wants one of its textures sampled.
//
// This belongs to the *shader*, not to the Texture, and that is a deliberate
// break from the obvious design. D3D12 offers two ways to give a draw its
// samplers: a descriptor table, which can vary per draw and would let the
// Texture carry its own state, or a static sampler baked into the root
// signature, which cannot. eacp used the descriptor table until a Windows-on-Arm
// driver turned out to ignore the table's offset outright and resolve every
// sampler to descriptor 0 of the heap - so every texture in the process sampled
// through whichever sampler happened to be first, and no per-texture state had
// any effect. Static samplers are unaffected, being nowhere near a heap.
//
// Since the configuration space is tiny, the root signature simply declares one
// static sampler for every (texture slot, configuration) pair and the emitter
// points each texture's sampler at the matching register. The cost is that the
// sampling is fixed when the shader is compiled rather than when a texture is
// bound, which is what this struct exists to say.
struct TextureSampling
{
    TextureFilter filter = TextureFilter::Nearest;
    TextureAddressMode addressMode = TextureAddressMode::Clamp;
};

// The number of distinct sampling configurations, and the index of one. The
// D3D12 root signature reserves this many static samplers per texture slot, and
// the register a texture's sampler lands on is slot * this + index.
constexpr int samplingConfigurations = 4;

constexpr int samplingIndex(const TextureSampling& sampling)
{
    return (sampling.filter == TextureFilter::Linear ? 2 : 0)
           + (sampling.addressMode == TextureAddressMode::Repeat ? 1 : 0);
}

// The value types the shader EDSL understands. Inputs, varyings and expression
// results are all described with these, and they spell identically in MSL and
// HLSL ("float2" etc.), so the emitters share one type vocabulary. UInt exists
// for the compute thread id and the element count it is checked against; it is
// never a vertex attribute.
enum class ValueType
{
    Float,
    Float2,
    Float3,
    Float4,
    Float4x4,
    UInt
};

constexpr int componentCount(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
        case ValueType::UInt:
            return 1;
        case ValueType::Float2:
            return 2;
        case ValueType::Float3:
            return 3;
        case ValueType::Float4:
            return 4;
        case ValueType::Float4x4:
            return 16;
    }

    return 1;
}

constexpr int byteSize(ValueType type)
{
    return componentCount(type) * 4;
}

inline const char* typeName(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
            return "float";
        case ValueType::Float2:
            return "float2";
        case ValueType::Float3:
            return "float3";
        case ValueType::Float4:
            return "float4";
        case ValueType::Float4x4:
            return "float4x4";
        case ValueType::UInt:
            return "uint";
    }

    return "float";
}
} // namespace eacp::GPU
