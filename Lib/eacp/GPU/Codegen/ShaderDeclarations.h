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

std::string_view getMetalTextureType(TextureKind kind);
std::string_view getHlslTextureType(TextureKind kind);
std::string formatGlslWritableTexture(int slot, int binding);
std::string
    formatGlslSampledTexture(const ShaderGraph& graph, int slot, int binding);
std::string formatHlslSamplerDeclarations(const ShaderGraph& graph);

std::string formatBoundsGuard(DispatchRank rank);
} // namespace eacp::GPU::ShaderEmission
