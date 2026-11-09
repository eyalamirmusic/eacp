#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>
#include <string>

// The values a pipeline specialises a library with: WGSL's pipeline-overridable
// constants, as the backends take them. One per constant the shader declares
// with a value other than its own default, keyed by the constant's id - its
// function_constant index on Metal, its constant_id on Vulkan, the
// EACP_OVERRIDE_<id> macro on D3D. A constant left out takes the default the
// shader declared, so the empty set is the library as compiled.
//
// The shader EDSL fills one from names (OverrideValues.h); a hand-written
// source names its own ids and marks itself ShaderSource::withPipelineConstants.

namespace eacp::GPU
{
enum class PipelineConstantType : std::uint8_t
{
    Bool,
    Int,
    UInt,
    Float,

    // An f16, held as the float it rounds to; asHalfBits() is its binary16.
    Half
};

struct PipelineConstant
{
    static PipelineConstant ofBool(int id, bool value);
    static PipelineConstant ofInt(int id, std::int32_t value);
    static PipelineConstant ofUInt(int id, std::uint32_t value);
    static PipelineConstant ofFloat(int id, float value);

    // Rounded to the nearest half first, as an f16 literal is.
    static PipelineConstant ofHalf(int id, float value);

    bool asBool() const;
    std::int32_t asInt() const;
    std::uint32_t asUInt() const;
    float asFloat() const;
    std::uint16_t asHalfBits() const;

    bool operator==(const PipelineConstant& other) const = default;

    int id = 0;
    PipelineConstantType type = PipelineConstantType::Float;

    // A Float or Half's float bits, an Int's two's complement, a Bool's 0 or 1.
    std::uint32_t bits = 0;
};

struct PipelineConstants
{
    bool isEmpty() const;

    // Adds or replaces the constant of that id, keeping the set in id order, so
    // two sets holding the same values compare equal.
    PipelineConstants& set(const PipelineConstant& constant);

    bool operator==(const PipelineConstants& other) const;

    Vector<PipelineConstant> values;
};

// HLSL has no specialisation, so a D3D library compiles a variant per set:
// the shader reads constant `id` through the macro EACP_OVERRIDE_<id>, and its
// source defines the default under #ifndef. These are the macros that replace
// them, each value spelled as an HLSL literal.
struct ShaderDefine
{
    std::string name;
    std::string value;
};

std::string hlslOverrideMacro(int id);
std::string hlslLiteralOf(const PipelineConstant& constant);
Vector<ShaderDefine> hlslDefinesFor(const PipelineConstants& constants);
} // namespace eacp::GPU
