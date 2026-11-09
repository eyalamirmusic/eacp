#pragma once

#include "ShaderDialect.h"

// How each dialect spells a shader struct (ShaderStruct.h) and reaches one in
// a buffer, all three at WGSL's storage offsets:
//
// - GLSL declares the struct as it is and its buffer as an std430 block of it,
//   whose layout rules are WGSL's own.
// - MSL lays the struct out to those offsets itself, where it is shareable: a
//   three-vector is its packed_ type, twelve bytes rather than sixteen, and a
//   uchar array stands in every gap and at the end, so a `device T*` indexes
//   it at the stride. A packed field is read back through its vector type.
// - HLSL declares the struct as a plain value type and reaches the buffer as
//   a [RW]ByteAddressBuffer at the offsets: D3D's StructuredBuffer packs a
//   three-vector to twelve bytes but aligns nothing past four, and SM5 has no
//   16-bit member at all, so its own layout could not hold WGSL's. A half is
//   read out of its word and written into it as HlslHalf buffers are, with
//   InterlockedAnd and InterlockedOr.
//
// MSL and HLSL build a struct value through a function per struct,
// eacpMake<Name>, since neither has a constructor for one; GLSL has its own.
// MSL's starts from a zeroed value, so a whole element written there stores
// its padding as zeroes rather than as whatever the register held.

namespace eacp::GPU::ShaderEmission
{
// Every struct the shader declared, in the order it declared them - which puts
// a nested struct ahead of the struct holding it.
std::string getStructDeclarations(const ShaderGraph& graph, Backend backend);

const std::string& getStructName(const ShaderGraph& graph, int structId);

std::string printStructConstruct(const ShaderGraph& graph,
                                 Backend backend,
                                 int structId,
                                 const std::string& members);

// A field of a struct held at `base`, read; and the same field written.
std::string printFieldRead(const ShaderGraph& graph,
                           Backend backend,
                           int structId,
                           int field,
                           const std::string& base);
std::string printFieldTarget(const ShaderGraph& graph,
                             int structId,
                             int field,
                             const std::string& base);

// A struct buffer's declared type: MSL's pointer, HLSL's byte buffer.
std::string getMetalStructBufferType(const ShaderGraph& graph,
                                     BufferAccess access,
                                     int slot);
std::string_view getHlslStructBufferType(BufferAccess access);

// HLSL: a field of element `index` of struct buffer `slot`, loaded from its
// bytes; and a store of `value` - an expression that may be read more than
// once - into a field of the element, or into the whole of it at field -1.
std::string printHlslStructLoad(const ShaderGraph& graph,
                                int slot,
                                const std::string& index,
                                int field);
std::string printHlslStructStore(const ShaderGraph& graph,
                                 int slot,
                                 const std::string& index,
                                 int field,
                                 const std::string& value,
                                 const std::string& indent);
} // namespace eacp::GPU::ShaderEmission
