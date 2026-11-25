#pragma once

#include "ShaderBindings.h"
#include "ShaderDialect.h"

namespace eacp::GPU::ShaderEmission
{
std::string_view getMetalBufferType(BufferAccess access, ValueType elementType);
std::string_view getHlslBufferType(BufferAccess access, ValueType elementType);

std::string formatUniformBlock(Backend backend,
                               const Vector<ValueType>& types,
                               const Vector<std::string>& names,
                               int binding = vulkanUniformBinding);

std::string formatGlslBufferBlock(BufferAccess access,
                                  ValueType elementType,
                                  int slot,
                                  int binding);

// The same three for slot `slot` of the graph, a buffer of structs included.
std::string
    getMetalSlotBufferType(const ShaderGraph& graph, BufferAccess access, int slot);
std::string
    getHlslSlotBufferType(const ShaderGraph& graph, BufferAccess access, int slot);
std::string formatGlslSlotBufferBlock(const ShaderGraph& graph,
                                      BufferAccess access,
                                      int slot,
                                      int binding);

std::string_view getMetalTextureType(TextureKind kind);
std::string_view getHlslTextureType(TextureKind kind);
// Any slot's declaration, sampled or storage (StorageTextures.h).
std::string getMetalTextureParameterType(const ShaderGraph& graph, int slot);
std::string getHlslTextureDeclarationType(const ShaderGraph& graph, int slot);
char getHlslTextureRegisterSpace(const ShaderGraph& graph, int slot);
std::string formatGlslTexture(const ShaderGraph& graph, int slot, int binding);

// A storage format's GLSL qualifier, its channel count, the type an HLSL
// typed UAV of it holds (one to four components) and what an SRV of it loads.
std::string_view getGlslImageFormat(TextureFormat format);
int getStorageChannelCount(TextureFormat format);
std::string getHlslStorageElementType(TextureFormat format);
std::string getHlslReadTexelType(TextureFormat format);
std::string formatHlslSamplerDeclarations(const ShaderGraph& graph);

// A sampler slot (Samplers.h) on HLSL: a SamplerState or a
// SamplerComparisonState at s<RenderPass::samplerRegisterBase + slot>.
std::string formatHlslSampler(const ShaderGraph& graph, int slot);

// Whether any function samples texture `slot` through a sampler slot. GLSL
// then also declares the slot's image on its own at the same binding, the
// texture<Kind> a sample constructs a sampler<Kind> from with the sampler.
bool isSampledThroughSampler(const ShaderGraph& graph, int slot);
std::string_view getGlslImageType(TextureKind kind);
std::string_view getGlslSampledType(TextureKind kind, bool comparison);

// WGSL's declarations at the bindings ShaderBindings.h names, one to a line: a
// storage buffer, and a texture slot with the sampler its sampling needs (or a
// sampler slot alone).
std::string
    formatWgslBuffer(const ShaderGraph& graph, BufferAccess access, int slot);
std::string formatWgslTexture(const ShaderGraph& graph, int slot);

// Braced in WGSL, which takes no unbraced body; alike in the other three.
std::string formatBoundsGuard(DispatchRank rank, Backend backend = Backend::Metal);
} // namespace eacp::GPU::ShaderEmission
