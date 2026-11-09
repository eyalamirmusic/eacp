#pragma once

#include "../Shader/PipelineConstants.h"
#include "ShaderGraph.h"

#include <string>
#include <string_view>

// The values a shader's overrides take for one pipeline, by name: WGSL's
// pipeline-overridable constants as GPUProgrammableStage::constants gives them.
//
//   auto gain = overrideConstant<Float>("gain", 1.f);    // in define()
//   auto taps = overrideConstant<UInt>("taps");          // no default
//
//   kernel.prepare(OverrideValues {}.set("taps", 4u));   // gain stays 1
//   kernel.specialize(OverrideValues {}.set("taps", 8u).set("gain", 0.5f));
//
// Each value is checked against the type its override was declared with: a
// float for a Float or a Half (rounded to the nearest half), an int for an Int,
// an unsigned for a UInt, a bool for a Bool - and an int or an unsigned for
// either integer type where the value is the same in both. A float has to be
// finite, and a Half's within the f16 range. A name the shader does not
// declare, a value of the wrong type and an override with no default given no
// value are each refused, naming the override.

namespace eacp::GPU
{
class OverrideValues
{
public:
    struct Entry
    {
        std::string name;
        PipelineConstant value;
    };

    // Setting a name again replaces its value.
    OverrideValues& set(std::string_view name, float value);
    OverrideValues& set(std::string_view name, double value);
    OverrideValues& set(std::string_view name, int value);
    OverrideValues& set(std::string_view name, unsigned value);
    OverrideValues& set(std::string_view name, bool value);
    OverrideValues& set(std::string_view name, const char* value) = delete;

    constexpr const Vector<Entry>& entries() const { return values; }

private:
    OverrideValues& setValue(std::string_view name, const PipelineConstant& value);

    Vector<Entry> values;
};

// What a set of values comes to against one shader: every override with the
// value it takes, given or default, in id order; the ones whose value is not
// their default, which is what a pipeline is specialised with and what tells
// one pipeline from another; or why the values do not fit the shader.
struct ResolvedOverrides
{
    bool isValid() const { return error.empty(); }

    Vector<PipelineConstant> values;
    PipelineConstants specialized;
    std::string error;
};

ResolvedOverrides resolveOverrides(const ShaderGraph& graph,
                                   const OverrideValues& values);

PipelineConstantType pipelineConstantTypeOf(ValueType type);
} // namespace eacp::GPU
