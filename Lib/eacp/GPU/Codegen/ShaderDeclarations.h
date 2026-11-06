#pragma once

#include "ShaderBindings.h"
#include "ShaderDialect.h"

// How each dialect declares what a shader binds: storage buffers, textures,
// samplers and the uniform block.

namespace eacp::GPU::ShaderEmission
{
const char* metalBufferType(BufferAccess access, ValueType elementType);
const char* hlslBufferType(BufferAccess access, ValueType elementType);
std::string bufferParameters(const ShaderGraph& graph, const Vector<int>& roots);

std::string uniformBlock(Backend backend,
                         const Vector<ValueType>& types,
                         const Vector<std::string>& names,
                         int binding = vulkanUniformBinding);

std::string glslBufferBlock(BufferAccess access,
                            ValueType elementType,
                            int slot,
                            int binding);

const char* metalTextureType(TextureKind kind);
const char* hlslTextureType(TextureKind kind);
std::string glslWritableTexture(int slot, int binding);
std::string glslSampledTexture(const ShaderGraph& graph, int slot, int binding);
std::string hlslSamplerDeclarations(const ShaderGraph& graph);

std::string boundsGuard(DispatchRank rank);
} // namespace eacp::GPU::ShaderEmission
