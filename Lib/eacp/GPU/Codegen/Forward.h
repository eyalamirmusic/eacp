#pragma once

#include <functional>

// Names from the shader codegen layer, for headers that mention them without
// needing the EDSL itself.

namespace eacp::GPU
{
class ShaderGraph;
class ShaderBuilder;
class ShaderProgram;
class ComputeKernel;
class ComputeProgram;

struct Float;
struct Float2;
struct Float3;
struct Float4;
struct UInt;
struct UInt2;
struct UInt3;
struct UInt4;
struct Int;
struct Int2;
struct Int3;
struct Int4;
struct Bool;
struct Bool2;
struct Bool3;
struct Bool4;
struct Float2x2;
struct Float3x3;
struct Float4x4;

// How a module hands over a shader whose program type is nested in a .cpp.
using ShaderGraphVisitor = std::function<void(const ShaderGraph&)>;
} // namespace eacp::GPU
