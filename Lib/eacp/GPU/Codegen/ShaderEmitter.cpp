#include "ShaderEmitter.h"

#include "../Frame/ComputePass.h"
#include "../Frame/RenderPass.h"
#include "ShaderBindings.h"
#include "ShaderGraph.h"
#include "UniformLayout.h"

#include <cassert>
#include <cstdio>

// The single source-of-truth walker. MSL and HLSL spell most of an expression
// identically; GLSL differs in a countable list, each with one arm here.

namespace eacp::GPU
{
namespace
{
enum class Backend
{
    Metal,
    DirectX,
    Vulkan
};

const char* typeName(Backend backend, ValueType type)
{
    return backend == Backend::Vulkan ? glslTypeName(type) : typeName(type);
}

std::string floatLiteral(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", value);

    auto text = std::string(buffer);

    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos
        && text.find('n') == std::string::npos)
        text += ".0";

    return text;
}

// GLSL rejects a non-flat integer stage input outright, HLSL wants
// nointerpolation and MSL [[flat]]; a float varying takes none of the three.
bool isFlatVarying(ValueType type)
{
    return isSignedInteger(type) || isUnsignedInteger(type);
}

std::string attributeSemantic(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return " [[attribute(" + std::to_string(index) + ")]]";

    return " : TEXCOORD" + std::to_string(index);
}

// Metal hangs the flat qualifier off the member rather than putting it in front
// of the type, so it rides here beside the semantic.
std::string varyingSemantic(Backend backend, int index, ValueType type)
{
    if (backend == Backend::Metal)
        return isFlatVarying(type) ? " [[flat]]" : std::string {};

    if (backend != Backend::DirectX)
        return {};

    return " : TEXCOORD" + std::to_string(index);
}

std::string positionSemantic(Backend backend)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return " [[position]]";

    return " : SV_Position";
}

std::string flatQualifier(Backend backend, ValueType type)
{
    if (backend == Backend::Metal || !isFlatVarying(type))
        return {};

    return backend == Backend::Vulkan ? "flat " : "nointerpolation ";
}

// Attribute i and varying i take location i, which is what the Vulkan
// vertex-input state and the vertex/fragment interface match on.
std::string locationLayout(int index)
{
    return "layout(location = " + std::to_string(index) + ") ";
}

// GLSL reads a stage's I/O out of globals, spelled attrN and varyN because aN
// and vN are taken there by a constant array and a mutable local.
std::string attributeName(Backend backend, int slot)
{
    if (backend == Backend::Vulkan)
        return "attr" + std::to_string(slot);

    return "input.a" + std::to_string(slot);
}

std::string varyingName(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return "vary" + std::to_string(index);

    return "input.v" + std::to_string(index);
}

// Call nodes carry the canonical (MSL) builtin name; renamed here per dialect.
std::string callName(Backend backend, const std::string& name)
{
    if (backend == Backend::DirectX)
    {
        if (name == "fract")
            return "frac";

        if (name == "mix")
            return "lerp";

        if (name == "dfdx")
            return "ddx";

        if (name == "dfdy")
            return "ddy";

        // One HLSL name per direction, whatever the width MSL named.
        if (name.starts_with("as_type<uint"))
            return "asuint";

        if (name.starts_with("as_type<float"))
            return "asfloat";
    }

    if (backend == Backend::Vulkan)
    {
        // A constructor-style cast is recorded as a call under the target's
        // canonical type name, so float2(v) leaves as vec2(v).
        if (const auto* spelling = glslTypeNameFor(name))
            return spelling;

        if (name == "atan2")
            return "atan";

        if (name == "rsqrt")
            return "inversesqrt";

        if (name == "dfdx")
            return "dFdx";

        if (name == "dfdy")
            return "dFdy";

        // Both are genType in GLSL, so one name per direction covers every
        // width MSL named.
        if (name.starts_with("as_type<uint"))
            return "floatBitsToUint";

        if (name.starts_with("as_type<float"))
            return "uintBitsToFloat";

        // GLSL has no log10; the helper table carries the definition.
        if (name == "log10")
            return "eacpLog10";
    }

    return name;
}

// The builtins GLSL overloads per width: it takes a scalar beside a vector only
// in a few trailing positions, where MSL converts and HLSL promotes it.
bool isGenTypeCall(const std::string& name)
{
    return name == "min" || name == "max" || name == "clamp" || name == "mix"
           || name == "step" || name == "smoothstep" || name == "pow"
           || name == "atan2";
}

// GLSL reserves the relational and equality operators for scalars, so a
// componentwise mask is a function there. Asked only for a boolean vector.
const char* glslComparison(const std::string& op)
{
    if (op == "<")
        return "lessThan";

    if (op == "<=")
        return "lessThanEqual";

    if (op == ">")
        return "greaterThan";

    if (op == ">=")
        return "greaterThanEqual";

    if (op == "==")
        return "equal";

    if (op == "!=")
        return "notEqual";

    return nullptr;
}

// The builtins whose two spellings are not one name apart, emitted as a
// function definition ahead of the shader body and called like any other.
//
// callName above handles the ordinary case - one name, one argument list, a
// different word. This is for the rest: unpacking two halves out of a word is
// a bitcast and a vector conversion on MSL, and two f16tof32 calls against a
// shift on HLSL, and no renaming reconciles those. A helper does, and it keeps
// the graph backend-agnostic - one call node with one argument, both sides.
//
// Each definition stands alone and calls no other helper, because what is
// emitted is decided per helper by whether the graph names it: one that leaned
// on another would compile only when the graph happened to call both.
struct ShaderHelper
{
    const char* name;

    // Null where the dialect has the function natively and emits nothing for it.
    const char* metal;
    const char* directX;
    const char* glsl;
};

// The error function, as Abramowitz & Stegun 7.1.26.
//
// Neither language has one. HLSL under FXC never did, and MSL - despite being
// the side that usually has the richer math library - rejects a call to erf as
// an undeclared identifier, so this is the definition on both backends rather
// than the Windows half of a pair.
//
// That is why one string serves both: what the approximation is written out of
// - abs, exp, a divide, a Horner chain and the scalar conditionals - is spelled
// identically in MSL and HLSL, so there is nothing here for a per-backend form
// to differ about. The vector widths are overloads rather than a genType
// because HLSL resolves a user function by overload and has no template before
// shader model 6; MSL is C++ and accepts the overloads unchanged.
//
// Measured against std::erf over the whole real line: worst absolute error
// under 6e-7 for both, and 1.7e-7 for what Metal's own arithmetic makes of it.
// Tests/GPU/IntrinsicTests pins both. The approximation itself is good to
// 1.5e-7 and the rest is what evaluating it in float32 costs - which lands
// under the resolution a float has near one either way, so the shader is a
// float32 error function and not a rounded copy of the CPU's.
//
// The polynomial is not odd, and both signs of zero take its positive branch,
// so the origin returns the argument: erf(0) is exactly zero with the sign it
// was handed, and erf(-x) is bitwise the negation of erf(x).
constexpr auto erfHelper =
    "float eacpErf(float x)\n"
    "{\n"
    "    float a = abs(x);\n"
    "    float t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    float e = 1.0 - t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return a == 0.0 ? x : (x < 0.0 ? -e : e);\n"
    "}\n\n"
    "float2 eacpErf(float2 x)\n"
    "{\n"
    "    return float2(eacpErf(x.x), eacpErf(x.y));\n"
    "}\n\n"
    "float3 eacpErf(float3 x)\n"
    "{\n"
    "    return float3(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z));\n"
    "}\n\n"
    "float4 eacpErf(float4 x)\n"
    "{\n"
    "    return float4(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z), eacpErf(x.w));\n"
    "}\n\n";

// Its complement, as poly(t) * exp(-x*x) rather than as 1 - eacpErf(x): erf has
// saturated at 1.0f by x = 4 while erfc there is still 1.5e-8, so the
// subtraction would return zero for the whole tail - which is the half of erfc
// that anything asks for it by name. What the direct form cannot fix is the
// approximation's own relative error out there, around 1% by x = 3, so this
// answers "how much probability is left" and not "to how many digits".
//
// The origin is answered outright for the same reason: erfc(0) is exactly 1,
// whichever sign of zero it was handed, so erf(x) + erfc(x) is exactly 1 there.
constexpr auto erfcHelper =
    "float eacpErfc(float x)\n"
    "{\n"
    "    float a = abs(x);\n"
    "    float t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    float e = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return a == 0.0 ? 1.0 : (x < 0.0 ? 2.0 - e : e);\n"
    "}\n\n"
    "float2 eacpErfc(float2 x)\n"
    "{\n"
    "    return float2(eacpErfc(x.x), eacpErfc(x.y));\n"
    "}\n\n"
    "float3 eacpErfc(float3 x)\n"
    "{\n"
    "    return float3(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z));\n"
    "}\n\n"
    "float4 eacpErfc(float4 x)\n"
    "{\n"
    "    return float4(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z), "
    "eacpErfc(x.w));\n"
    "}\n\n";

constexpr auto erfHelperGlsl =
    "float eacpErf(float x)\n"
    "{\n"
    "    float a = abs(x);\n"
    "    float t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    float e = 1.0 - t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return a == 0.0 ? x : (x < 0.0 ? -e : e);\n"
    "}\n\n"
    "vec2 eacpErf(vec2 x)\n"
    "{\n"
    "    return vec2(eacpErf(x.x), eacpErf(x.y));\n"
    "}\n\n"
    "vec3 eacpErf(vec3 x)\n"
    "{\n"
    "    return vec3(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z));\n"
    "}\n\n"
    "vec4 eacpErf(vec4 x)\n"
    "{\n"
    "    return vec4(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z), eacpErf(x.w));\n"
    "}\n\n";

constexpr auto erfcHelperGlsl =
    "float eacpErfc(float x)\n"
    "{\n"
    "    float a = abs(x);\n"
    "    float t = 1.0 / (1.0 + 0.3275911 * a);\n"
    "    float e = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741\n"
    "              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);\n"
    "    return a == 0.0 ? 1.0 : (x < 0.0 ? 2.0 - e : e);\n"
    "}\n\n"
    "vec2 eacpErfc(vec2 x)\n"
    "{\n"
    "    return vec2(eacpErfc(x.x), eacpErfc(x.y));\n"
    "}\n\n"
    "vec3 eacpErfc(vec3 x)\n"
    "{\n"
    "    return vec3(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z));\n"
    "}\n\n"
    "vec4 eacpErfc(vec4 x)\n"
    "{\n"
    "    return vec4(eacpErfc(x.x), eacpErfc(x.y), eacpErfc(x.z), "
    "eacpErfc(x.w));\n"
    "}\n\n";

// log2 scaled by log10(2), which is what a driver's own log10 lowers to.
constexpr auto log10HelperGlsl = "float eacpLog10(float x)\n"
                                 "{\n"
                                 "    return log2(x) * 0.30102999566;\n"
                                 "}\n\n"
                                 "vec2 eacpLog10(vec2 x)\n"
                                 "{\n"
                                 "    return log2(x) * 0.30102999566;\n"
                                 "}\n\n"
                                 "vec3 eacpLog10(vec3 x)\n"
                                 "{\n"
                                 "    return log2(x) * 0.30102999566;\n"
                                 "}\n\n"
                                 "vec4 eacpLog10(vec4 x)\n"
                                 "{\n"
                                 "    return log2(x) * 0.30102999566;\n"
                                 "}\n\n";

const auto shaderHelpers = Array<ShaderHelper, 6> {
    ShaderHelper {"eacpUnpackHalf2",
                  "inline float2 eacpUnpackHalf2(uint bits)\n"
                  "{\n"
                  "    return float2(as_type<half2>(bits));\n"
                  "}\n\n",
                  "float2 eacpUnpackHalf2(uint bits)\n"
                  "{\n"
                  "    return float2(f16tof32(bits), f16tof32(bits >> 16));\n"
                  "}\n\n",
                  "vec2 eacpUnpackHalf2(uint bits)\n"
                  "{\n"
                  "    return unpackHalf2x16(bits);\n"
                  "}\n\n"},
    ShaderHelper {"eacpErf", erfHelper, erfHelper, erfHelperGlsl},
    ShaderHelper {"eacpErfc", erfcHelper, erfcHelper, erfcHelperGlsl},
    // One half chosen by a parity rather than both unpacked and one dropped:
    // shifting the wanted half down is a single instruction in every language.
    // as_type<half2>, f16tof32 and unpackHalf2x16 all read the low sixteen bits.
    ShaderHelper {"eacpReadHalf",
                  "inline float eacpReadHalf(uint bits, uint parity)\n"
                  "{\n"
                  "    return float(as_type<half2>(bits >> (16u * parity)).x);\n"
                  "}\n\n",
                  "float eacpReadHalf(uint bits, uint parity)\n"
                  "{\n"
                  "    return f16tof32(bits >> (16u * parity));\n"
                  "}\n\n",
                  "float eacpReadHalf(uint bits, uint parity)\n"
                  "{\n"
                  "    return unpackHalf2x16(bits >> (16u * parity)).x;\n"
                  "}\n\n"},

    // The narrowing. No mask on the low half: f32tof16 is specified to set the
    // upper sixteen bits of its result to zero, and the D3D11.1 spec says so
    // again as a clarification that it holds on all hardware supporting the
    // instruction.
    //
    // Not bit-identical across the three for a value fp16 cannot hold.
    ShaderHelper {"eacpPackHalf2",
                  "inline uint eacpPackHalf2(float2 values)\n"
                  "{\n"
                  "    return as_type<uint>(half2(values));\n"
                  "}\n\n",
                  "uint eacpPackHalf2(float2 values)\n"
                  "{\n"
                  "    return f32tof16(values.x) | (f32tof16(values.y) << 16u);\n"
                  "}\n\n",
                  "uint eacpPackHalf2(vec2 values)\n"
                  "{\n"
                  "    return packHalf2x16(values);\n"
                  "}\n\n"},

    ShaderHelper {"log10", nullptr, nullptr, log10HelperGlsl}};

const char* helperDefinition(const ShaderHelper& helper, Backend backend)
{
    switch (backend)
    {
        case Backend::Metal:
            return helper.metal;
        case Backend::DirectX:
            return helper.directX;
        case Backend::Vulkan:
            return helper.glsl;
    }

    return helper.metal;
}

// Only the helpers a graph actually calls, so a shader that unpacks nothing
// carries no definition for one.
std::string helperDefinitions(const ShaderGraph& graph, Backend backend)
{
    auto definitions = std::string {};

    for (const auto& helper: shaderHelpers)
    {
        auto used = false;

        for (auto node = 0; node < graph.nodeCount() && !used; ++node)
        {
            const auto& expr = graph.expr(node);
            used = expr.kind == ExprKind::Call && expr.text == helper.name;
        }

        if (!used)
            continue;

        if (const auto* definition = helperDefinition(helper, backend))
            definitions += definition;
    }

    return definitions;
}

// The HLSL sampler a texture with this sampling reads through. Named for the
// configuration rather than for the texture, because that is what it is: the
// root signature declares samplingConfigurations static samplers and every
// texture sampled that way shares one. See TextureSampling.
std::string hlslSamplerName(const TextureSampling& sampling)
{
    return "samplerConfig" + std::to_string(samplingIndex(sampling));
}

// The component a 2D or 3D kernel's thread index node asked for, and the
// uniform its matching grid extent is declared under.
const char* componentSuffix(int component)
{
    if (component == 0)
        return ".x";

    return component == 1 ? ".y" : ".z";
}

// A thread index under the name both kernel scaffoldings bind it to: the whole
// value where the node took the position as one, one lane otherwise.
std::string indexReference(const char* name, DispatchRank rank, int component)
{
    if (rank == DispatchRank::OneD || component == allComponents)
        return name;

    return name + std::string(componentSuffix(component));
}

const char* gridExtentName(int component)
{
    if (component == 0)
        return "width";

    return component == 1 ? "height" : "depth";
}

int gridExtentCount(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return 1;

    return rank == DispatchRank::TwoD ? 2 : 3;
}

// The type the entry point declares its indices as, and the swizzle HLSL and
// GLSL take them out of their three-component builtins with.
const char* indexTypeName(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "uint";

    return rank == DispatchRank::TwoD ? "uint2" : "uint3";
}

const char* glslIndexTypeName(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "uint";

    return rank == DispatchRank::TwoD ? "uvec2" : "uvec3";
}

const char* indexSwizzle(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return ".x";

    return rank == DispatchRank::TwoD ? ".xy" : ".xyz";
}

// How many threads one group holds, whatever its rank - what a group reduction
// folds over and what its scratch is sized for.
int threadsPerGroup(const ShaderGraph& graph)
{
    return graph.threadGroupShape().threadCount();
}

// The first halving step of a tree over that many threads: the largest power of
// two below the count, so a count that is not one still folds its tail in first.
int reductionStride(int threads)
{
    auto stride = 1;

    while (stride * 2 < threads)
        stride *= 2;

    return stride;
}

// The scratch a reduction stages its partials in, one array per element type
// folded. Named rather than slotted because it is the emitter's own and not
// something the kernel declared.
const char* groupScratchName(ValueType elementType)
{
    return elementType == ValueType::UInt ? "groupScratchU" : "groupScratch";
}

// The MSL SIMD-group reduction each fold starts from.
const char* metalSimdReduction(GroupReduction operation)
{
    switch (operation)
    {
        case GroupReduction::Sum:
            return "simd_sum";
        case GroupReduction::Max:
            return "simd_max";
        case GroupReduction::Min:
            return "simd_min";
    }

    return "simd_sum";
}

// Two partials combined, spelled identically in all three dialects.
std::string foldedPair(GroupReduction operation,
                       const std::string& left,
                       const std::string& right)
{
    switch (operation)
    {
        case GroupReduction::Sum:
            return left + " + " + right;
        case GroupReduction::Max:
            return "max(" + left + ", " + right + ")";
        case GroupReduction::Min:
            return "min(" + left + ", " + right + ")";
    }

    return left + " + " + right;
}

// The barrier itself, shared by the barrier statement and by the reductions
// that bracket their scratch with one.
std::string barrierStatement(Backend backend, const std::string& indent)
{
    if (backend == Backend::Vulkan)
        return indent + "memoryBarrierShared();\n" + indent + "barrier();\n";

    return indent
           + std::string(backend == Backend::Metal
                             ? "threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                             : "GroupMemoryBarrierWithGroupSync();\n");
}

// The flat position of a thread within its group, which the tree indexes its
// scratch by. Both dialects that take the tree have a builtin for it, so
// neither derives one from the three-component local id.
const char* groupLaneName(Backend backend)
{
    return backend == Backend::Vulkan ? "gl_LocalInvocationIndex" : "groupLane";
}

// A SIMD-group matrix fragment's name. Its own numbering, so it collides with
// neither the variables nor the shared arrays.
std::string simdMatrixName(int slot)
{
    return "sgm" + std::to_string(slot);
}

// Where a fragment's patch lives, spelled as the emitted source names it.
std::string simdMatrixMemoryName(const Statement& statement)
{
    return (statement.memory == SimdMatrixMemory::Shared ? "s" : "buffer")
           + std::to_string(statement.bufferSlot);
}

// A whole expression parenthesised, since an offset or a stride is printed
// into the middle of an index computation.
std::string bracketed(const std::string& expression)
{
    return "(" + expression + ")";
}

// The lane a fallback fragment's store is made by. There is no SIMD group on
// the backends that take the fallback, so every thread of what would be one
// holds its own copy of the fragment and computes it redundantly; letting all
// of them write would be the same bytes 32 times over and a race to say it
// with, so the first lane of each writes and the rest do not.
std::string simdLeadLane(Backend backend)
{
    return std::string(groupLaneName(backend)) + " % "
           + std::to_string(simdGroupWidth) + "u == 0u";
}

// Prints one stage's expressions. Nodes the stage plan named as locals print
// as tN references; everything else prints inline. print() spells out a node's
// own expression (used for both inline nodes and local definitions), ref() is
// what children and outputs go through, so shared subtrees collapse to a name.
struct ExprPrinter
{
    // -1 when the stage reads the attribute itself; empty outside the fragment
    // stage.
    int carryingVarying(int slot) const
    {
        return slot < attributeVaryings.size() ? attributeVaryings[slot] : -1;
    }

    std::string ref(int node) const
    {
        if (locals[node] >= 0)
            return "t" + std::to_string(locals[node]);

        return print(node);
    }

    // Float, whose width is one, when nothing is to be broadcast.
    ValueType broadcastType(const Expr& call) const
    {
        if (backend != Backend::Vulkan || !isGenTypeCall(call.text))
            return ValueType::Float;

        auto widest = ValueType::Float;

        for (auto argument: call.args)
        {
            auto type = graph.expr(argument).type;

            if (componentCount(type) > componentCount(widest))
                widest = type;
        }

        return widest;
    }

    std::string widened(int node, ValueType wide) const
    {
        auto argument = ref(node);

        if (componentCount(wide) == 1 || componentCount(graph.expr(node).type) > 1)
            return argument;

        return std::string(typeName(backend, wide)) + "(" + argument + ")";
    }

    std::string print(int node) const
    {
        const auto& expr = graph.expr(node);

        switch (expr.kind)
        {
            // Only the vertex stage is handed the attributes, so a
            // fragment-stage read of one reads the varying carrying it across.
            case ExprKind::Input:
            {
                auto carrier = carryingVarying(expr.index);

                if (carrier >= 0)
                    return varyingName(backend, carrier);

                return attributeName(backend, expr.index);
            }

            case ExprKind::Varying:
                return varyingName(backend, expr.index);

            case ExprKind::Uniform:
                return "uniforms.u" + std::to_string(expr.index);

            case ExprKind::Constant:
                // The uint, int and bool spellings are shared by MSL and HLSL,
                // like floatN. A signed literal needs no suffix at all: an
                // integer literal is already an int in both languages.
                //
                // Expr::index is the int the three of them share, so a uint
                // above INT_MAX is held there as a negative and has to be read
                // back as what it was: 4294967295u, never -1u.
                if (expr.type == ValueType::UInt)
                    return std::to_string((unsigned) expr.index) + "u";

                if (expr.type == ValueType::Int)
                    return std::to_string(expr.index);

                if (expr.type == ValueType::Bool)
                    return expr.index != 0 ? "true" : "false";

                return floatLiteral(expr.value);

            case ExprKind::Construct:
            {
                auto text = std::string(typeName(backend, expr.type)) + "(";

                for (auto i = 0; i < expr.args.size(); ++i)
                {
                    if (i > 0)
                        text += ", ";

                    text += ref(expr.args[i]);
                }

                text += ")";

                // MSL and GLSL fill a matrix from columns, HLSL from rows, so
                // transpose() is what restores the column-major value there.
                if (backend == Backend::DirectX && isMatrix(expr.type))
                    return "transpose(" + text + ")";

                return text;
            }

            case ExprKind::Swizzle:
                return "(" + ref(expr.args[0]) + ")." + expr.text;

            case ExprKind::Call:
            {
                auto text = callName(backend, expr.text) + "(";
                auto wide = broadcastType(expr);

                for (auto i = 0; i < expr.args.size(); ++i)
                {
                    if (i > 0)
                        text += ", ";

                    text += widened(expr.args[i], wide);
                }

                return text + ")";
            }

            case ExprKind::Unary:
                // GLSL gives ! to a scalar bool only; the componentwise
                // negation of a mask is not().
                if (backend == Backend::Vulkan && expr.op == '!'
                    && componentCount(expr.type) > 1)
                    return "not(" + ref(expr.args[0]) + ")";

                // The operand gets its own parentheses: negating a negative
                // constant must print (-(-1.0)), never the pre-decrement
                // (--1.0).
                return "(" + std::string(1, expr.op) + "(" + ref(expr.args[0])
                       + "))";

            case ExprKind::Binary:
            {
                // The operator is a char unless it did not fit in one, which is
                // only the two shifts.
                auto op = expr.text.empty() ? std::string(1, expr.op) : expr.text;
                auto isShift = op == "<<" || op == ">>";

                // GLSL refuses a scalar on the left of a shift whose right
                // operand is a vector; MSL and HLSL broadcast it themselves.
                auto left = backend == Backend::Vulkan && isShift
                                ? widened(expr.args[0], expr.type)
                                : ref(expr.args[0]);
                auto right = ref(expr.args[1]);

                // GLSL leaves % undefined on a negative operand, so the
                // truncating remainder is written out of the division.
                if (backend == Backend::Vulkan && op == "%"
                    && isSignedInteger(expr.type))
                    return "(" + left + " - ((" + left + " / " + right + ") * "
                           + right + "))";

                return "(" + left + " " + op + " " + right + ")";
            }

            case ExprKind::Compare:
            {
                // A mask is the operator in two dialects, a function in GLSL.
                if (backend == Backend::Vulkan && componentCount(expr.type) > 1)
                    if (const auto* name = glslComparison(expr.text))
                        return std::string(name) + "(" + ref(expr.args[0]) + ", "
                               + ref(expr.args[1]) + ")";

                return "(" + ref(expr.args[0]) + " " + expr.text + " "
                       + ref(expr.args[1]) + ")";
            }

            case ExprKind::Select:
                // Both languages spell the conditional operator the same way,
                // and both evaluate it without branching for scalar operands.
                return "(" + ref(expr.args[0]) + " ? " + ref(expr.args[1]) + " : "
                       + ref(expr.args[2]) + ")";

            case ExprKind::VarRead:
                return "v" + std::to_string(expr.index);

            case ExprKind::Mul:
            {
                // MSL and GLSL spell a matrix product with *; HLSL uses mul().
                // All three read a vector on the left of one as a row.
                auto left = ref(expr.args[0]);
                auto right = ref(expr.args[1]);

                if (backend != Backend::DirectX)
                    return "(" + left + " * " + right + ")";

                return "mul(" + left + ", " + right + ")";
            }

            case ExprKind::Sample:
            {
                // Texture sample at a float2 coordinate. A second argument is
                // the mip level the shader picked, which each backend spells
                // its own way: Metal as an extra argument to the same call,
                // HLSL as a different method.
                //
                // The two backends also name the sampler differently, and that
                // is the one place their declarations genuinely differ. MSL
                // passes a sampler as a function argument, so there is one per
                // texture and it carries the texture's index; HLSL binds one to
                // a register, and there is one per sampling configuration that
                // every texture declaring that sampling shares. See
                // TextureSampling.
                auto name = "texture" + std::to_string(expr.index);
                auto sampler =
                    backend == Backend::Metal
                        ? "sampler" + std::to_string(expr.index)
                        : hlslSamplerName(graph.textureSampling(expr.index));
                auto uv = ref(expr.args[0]);

                // GLSL's sampler2D over a depth image hands back four
                // channels, so the .r is the one float the node's type says.
                if (backend == Backend::Vulkan)
                {
                    auto call = expr.args.size() < 2
                                    ? "texture(" + name + ", " + uv + ")"
                                    : "textureLod(" + name + ", " + uv + ", "
                                          + ref(expr.args[1]) + ")";

                    if (graph.textureKind(expr.index) == TextureKind::Depth2D)
                        return call + ".r";

                    return call;
                }

                if (expr.args.size() < 2)
                {
                    auto method =
                        backend == Backend::Metal ? ".sample(" : ".Sample(";

                    return name + method + sampler + ", " + uv + ")";
                }

                auto level = ref(expr.args[1]);

                if (backend == Backend::Metal)
                    return name + ".sample(" + sampler + ", " + uv + ", level("
                           + level + "))";

                return name + ".SampleLevel(" + sampler + ", " + uv + ", " + level
                       + ")";
            }

            case ExprKind::Fetch:
            {
                // A texel read at integer coordinates. Metal takes them
                // unsigned, so anything not already signed-integer goes through
                // int2 first: a negative coordinate then wraps to a large
                // unsigned one and reads as zero, which is what HLSL's Load does
                // with it directly. The level is 0 - GPU::Texture has no mips -
                // and D3D carries it in the coordinate's third component.
                auto name = "texture" + std::to_string(expr.index);
                auto given = ref(expr.args[0]);
                auto signedPair = std::string(typeName(backend, ValueType::Int2));
                auto coordinates = graph.expr(expr.args[0]).type == ValueType::Int2
                                       ? given
                                       : signedPair + "(" + given + ")";

                if (backend == Backend::Metal)
                    return name + ".read(uint2(" + coordinates + "))";

                // GLSL takes the coordinate signed and the level explicitly -
                // there are no derivatives to pick one from in a fetch.
                if (backend == Backend::Vulkan)
                    return "texelFetch(" + name + ", " + coordinates + ", 0)";

                return name + ".Load(int3(" + coordinates + ", 0))";
            }

            case ExprKind::ThreadId:
                // Both kernel scaffoldings declare the work-item id as gid: a
                // uint over the flat count in a 1D kernel, a uint2 or uint3
                // over the grid otherwise, where the node carries which
                // component it asked for - or the whole of it.
                return indexReference("gid", graph.dispatchRank(), expr.index);

            case ExprKind::BufferRead:
                return "buffer" + std::to_string(expr.index) + "["
                       + ref(expr.args[0]) + "]";

            case ExprKind::AtomicLoad:
            {
                // HLSL has nothing to spell: a UAV element of an
                // RWStructuredBuffer<uint> is already the thing an interlocked
                // operation acts on, and reading one is a subscript. MSL wraps
                // its atomic_uint, so the value has to be taken out of it.
                auto element = "buffer" + std::to_string(expr.index) + "["
                               + ref(expr.args[0]) + "]";

                if (backend == Backend::Metal)
                    return "atomic_load_explicit(&" + element
                           + ", memory_order_relaxed)";

                return element;
            }

            case ExprKind::ArrayRead:
                return "a" + std::to_string(expr.index) + "[" + ref(expr.args[0])
                       + "]";

            // The threadgroup indices ride the same scaffolding as gid: both
            // backends' entry points bind them to these names, a scalar in a
            // 1D kernel and a vector of the rank's width otherwise.
            case ExprKind::LocalId:
                return indexReference("lid", graph.dispatchRank(), expr.index);

            case ExprKind::GroupId:
                return indexReference("tgid", graph.dispatchRank(), expr.index);

            // The implicit bound the dispatch appended to the uniform block,
            // under the names the block declares it with.
            case ExprKind::GridExtent:
                if (graph.dispatchRank() == DispatchRank::OneD)
                    return "uniforms.count";

                return std::string("uniforms.") + gridExtentName(expr.index);

            case ExprKind::SharedRead:
                return "s" + std::to_string(expr.index) + "[" + ref(expr.args[0])
                       + "]";

            // Metal has the builtin; the other two divide the flat local index
            // by the width, which is the same numbering and the one the
            // fallback's own arithmetic is written against.
            case ExprKind::SimdGroupIndex:
                if (backend == Backend::Metal)
                    return "simdIndex";

                return "(" + std::string(groupLaneName(backend)) + " / "
                       + std::to_string(simdGroupWidth) + "u)";
        }

        return {};
    }

    const ShaderGraph& graph;
    Backend backend;
    const Vector<int>& locals; // node id -> local index, -1 = inline
    Vector<int> attributeVaryings; // attribute slot -> varying, -1 = read direct
};

// Operation nodes are worth naming when evaluated more than once; leaf reads
// and swizzles stay inline - naming them saves nothing and hurts readability.
// The one thing named whatever its kind is a record write's value, which its
// element stores all have to be handed rather than evaluate one at a time.
bool wantsLocal(ExprKind kind)
{
    switch (kind)
    {
        case ExprKind::Construct:
        case ExprKind::Call:
        case ExprKind::Unary:
        case ExprKind::Binary:
        case ExprKind::Compare:
        case ExprKind::Select:
        case ExprKind::Mul:
        case ExprKind::Sample:
        case ExprKind::Fetch:
        case ExprKind::BufferRead:
        case ExprKind::AtomicLoad:
        case ExprKind::ArrayRead:
        case ExprKind::SharedRead:
            return true;

        case ExprKind::Input:
        case ExprKind::Varying:
        case ExprKind::Uniform:
        case ExprKind::Constant:
        case ExprKind::Swizzle:
        case ExprKind::VarRead:
        case ExprKind::ThreadId:
        case ExprKind::LocalId:
        case ExprKind::GroupId:
        case ExprKind::GridExtent:
        case ExprKind::SimdGroupIndex:
            return false;
    }

    return false;
}

// Counts how many references each node receives across the stage's roots: one
// per root plus one per parent edge, visiting each node's children only once.
void countUses(const ShaderGraph& graph,
               int node,
               Vector<int>& uses,
               Vector<char>& seen)
{
    if (node < 0)
        return;

    ++uses[node];

    if (seen[node])
        return;

    seen[node] = 1;

    for (auto argument: graph.expr(node).args)
        countUses(graph, argument, uses, seen);
}

// Which nodes a run of expressions evaluates more than once, in dependency
// (post) order so every definition precedes its uses. A node that already holds
// a name is left alone, and so is everything under it: it is already computed.
void orderLocals(const ShaderGraph& graph,
                 int node,
                 const Vector<int>& uses,
                 const Vector<int>& locals,
                 Vector<char>& seen,
                 Vector<int>& order)
{
    if (node < 0 || seen[node] || locals[node] >= 0)
        return;

    seen[node] = 1;

    for (auto argument: graph.expr(node).args)
        orderLocals(graph, argument, uses, locals, seen, order);

    if (uses[node] > 1 && wantsLocal(graph.expr(node).kind))
        order.add(node);
}

// Which constant arrays a run of expressions subscripts, following the elements
// of one that is used in case an element subscripts another.
void collectArrays(const ShaderGraph& graph,
                   int node,
                   Vector<char>& used,
                   Vector<char>& seen)
{
    if (node < 0 || seen[node])
        return;

    seen[node] = 1;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::ArrayRead && used[expr.index] == 0)
    {
        used[expr.index] = 1;

        for (auto element: graph.arrays()[expr.index].elements)
            collectArrays(graph, element, used, seen);
    }

    for (auto argument: expr.args)
        collectArrays(graph, argument, used, seen);
}

// Which variables running a statement can leave holding something else -
// following the bodies of an if or a loop, since what they write is written
// just the same.
void collectWrites(const ShaderGraph& graph, int block, Vector<char>& written);

void collectWrites(const ShaderGraph& graph,
                   const Statement& statement,
                   Vector<char>& written)
{
    switch (statement.kind)
    {
        case StatementKind::Declare:
        case StatementKind::Assign:
        case StatementKind::AtomicAdd:
        case StatementKind::GroupReduce:
            written[statement.slot] = 1;
            return;

        case StatementKind::If:
            collectWrites(graph, statement.body, written);

            if (statement.elseBody >= 0)
                collectWrites(graph, statement.elseBody, written);

            return;

        case StatementKind::Loop:
            collectWrites(graph, statement.body, written);
            return;

        case StatementKind::Break:
        case StatementKind::Continue:
        case StatementKind::Store:
        case StatementKind::TextureStore:
        case StatementKind::SharedStore:
        case StatementKind::Barrier:
        case StatementKind::SimdMatrixFill:
        case StatementKind::SimdMatrixLoad:
        case StatementKind::SimdMatrixStore:
        case StatementKind::SimdMatrixMultiplyAdd:
            return;
    }
}

// Whether running a statement can change what threadgroup memory holds: a
// store to it, or the barrier that publishes what other threads stored -
// following nested bodies the way collectWrites does. What this feeds is the
// same rule variables get: a name computed from shared memory is given up the
// moment shared memory may have moved on.
bool touchesShared(const ShaderGraph& graph, int block);

bool touchesShared(const ShaderGraph& graph, const Statement& statement)
{
    switch (statement.kind)
    {
        case StatementKind::SharedStore:
        case StatementKind::Barrier:
        case StatementKind::GroupReduce:
            return true;

        // A fragment stored back into a threadgroup tile moves that tile, so
        // any name read out of it beforehand is given up here; one stored into
        // a buffer moves no shared memory at all.
        case StatementKind::SimdMatrixStore:
            return statement.memory == SimdMatrixMemory::Shared;

        case StatementKind::If:
            if (touchesShared(graph, statement.body))
                return true;

            return statement.elseBody >= 0
                   && touchesShared(graph, statement.elseBody);

        case StatementKind::Loop:
            return touchesShared(graph, statement.body);

        case StatementKind::Declare:
        case StatementKind::Assign:
        case StatementKind::Break:
        case StatementKind::Continue:
        case StatementKind::Store:
        case StatementKind::TextureStore:
        case StatementKind::AtomicAdd:
        case StatementKind::SimdMatrixFill:
        case StatementKind::SimdMatrixLoad:
        case StatementKind::SimdMatrixMultiplyAdd:
            return false;
    }

    return false;
}

bool touchesShared(const ShaderGraph& graph, int block)
{
    for (auto index: graph.block(block).statements)
        if (touchesShared(graph, graph.statement(index)))
            return true;

    return false;
}

void collectWrites(const ShaderGraph& graph, int block, Vector<char>& written)
{
    for (auto index: graph.block(block).statements)
        collectWrites(graph, graph.statement(index), written);
}

// Its storage-buffer sibling: which buffer slots running a statement can leave
// holding something else. What it feeds is the rule variables and shared memory
// already get - a name computed from an element is given up the moment that
// buffer may have moved on - and it is what makes an output a kernel reads back
// answer with what the kernel stored rather than with what was there before.
void collectBufferWrites(const ShaderGraph& graph, int block, Vector<char>& written);

void collectBufferWrites(const ShaderGraph& graph,
                         const Statement& statement,
                         Vector<char>& written)
{
    switch (statement.kind)
    {
        case StatementKind::Store:
            written[statement.slot] = 1;
            return;

        // The one statement whose buffer is not in `slot`: that field names the
        // variable the value from before the add lands in.
        case StatementKind::AtomicAdd:
            written[statement.bufferSlot] = 1;
            return;

        // Nor is a fragment store's, `slot` there naming the fragment. It
        // writes a buffer only when that is where its patch is.
        case StatementKind::SimdMatrixStore:
            if (statement.memory == SimdMatrixMemory::Buffer)
                written[statement.bufferSlot] = 1;

            return;

        case StatementKind::If:
            collectBufferWrites(graph, statement.body, written);

            if (statement.elseBody >= 0)
                collectBufferWrites(graph, statement.elseBody, written);

            return;

        case StatementKind::Loop:
            collectBufferWrites(graph, statement.body, written);
            return;

        case StatementKind::Declare:
        case StatementKind::Assign:
        case StatementKind::Break:
        case StatementKind::Continue:
        case StatementKind::TextureStore:
        case StatementKind::SharedStore:
        case StatementKind::Barrier:
        case StatementKind::GroupReduce:
        case StatementKind::SimdMatrixFill:
        case StatementKind::SimdMatrixLoad:
        case StatementKind::SimdMatrixMultiplyAdd:
            return;
    }
}

void collectBufferWrites(const ShaderGraph& graph, int block, Vector<char>& written)
{
    for (auto index: graph.block(block).statements)
        collectBufferWrites(graph, graph.statement(index), written);
}

// A visited set a walk can have a fresh one of without paying for one. Marking
// is a stamp rather than a flag, so starting over is a counter increment
// instead of clearing a buffer the size of the graph.
//
// It exists because the walk below runs per open name per statement, and a
// buffer allocated and zeroed each time costs the whole graph however small the
// subtree walked turns out to be. On an ordinary shader that is invisible; on a
// large one it is the difference between a shader that compiles and an app that
// hangs.
struct VisitSet
{
    explicit VisitSet(int nodeCount) { stamps.resize(nodeCount, 0); }

    void restart() { ++generation; }

    bool visit(int node)
    {
        if (stamps[node] == generation)
            return false;

        stamps[node] = generation;
        return true;
    }

    Vector<int> stamps;

    // Ahead of the stamps a fresh buffer holds, so nothing counts as visited
    // until something visits it. Starting level with them makes every node of a
    // new set look already seen - which is not a walk that gives the wrong
    // answer slowly, it is one that gives it immediately.
    int generation = 1;
};

// Whether the value under node no longer stands for itself after a statement:
// it read a variable that statement wrote, an element of a storage buffer the
// statement stored to, or threadgroup memory the statement may have moved.
bool readsStale(const ShaderGraph& graph,
                int node,
                const Vector<char>& written,
                const Vector<char>& buffersWritten,
                bool sharedMoved,
                VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return false;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::VarRead && written[expr.index] != 0)
        return true;

    if (sharedMoved && expr.kind == ExprKind::SharedRead)
        return true;

    // Whichever way the buffer was declared: an output a kernel reads back and
    // an atomic counter it loads are both elements a store can have changed.
    if ((expr.kind == ExprKind::BufferRead || expr.kind == ExprKind::AtomicLoad)
        && buffersWritten[expr.index] != 0)
        return true;

    for (auto argument: expr.args)
        if (readsStale(graph, argument, written, buffersWritten, sharedMoved, seen))
            return true;

    return false;
}

// Every expression the statements of a block reach, its nested bodies included.
// A loop's condition is left out: the header takes no name of its own.
void collectUseRoots(const ShaderGraph& graph, int block, Vector<int>& roots)
{
    for (auto index: graph.block(block).statements)
    {
        const auto& statement = graph.statement(index);

        if (statement.kind != StatementKind::Loop)
            roots.add(statement.value);

        roots.add(statement.index);
        roots.add(statement.indexY);
        roots.add(statement.stride);

        if (statement.body >= 0)
            collectUseRoots(graph, statement.body, roots);

        if (statement.elseBody >= 0)
            collectUseRoots(graph, statement.elseBody, roots);
    }
}

// Emits one stage: its statements, then the expressions its outputs are.
//
// Any operation evaluated more than once becomes a tN local, so a shared
// subtree is computed - and printed - once instead of being inlined at every
// use. Control flow is what bounds that sharing, and the two rules it imposes
// are the whole of what makes this different from printing an expression tree:
//
// A name is given up the moment a statement writes a variable the value behind
// it read - which is what stops `d` computed before an `if` from standing for
// the same thing after a body that moved what it was computed from. A name
// neither body moves stays usable inside them both.
//
// A loop condition takes no name at all. It is printed into the while header,
// so binding it to a local ahead of the loop would test a value that never
// changes again; the names the body can invalidate are given up there too,
// since the header is re-evaluated after the body has run.
//
// A record write is what the rules answer to rather than bound by: its N
// element stores are one write, so its value takes a name whatever its use
// count and keeps it until the last of them has run.
struct StageEmitter
{
    StageEmitter(const ShaderGraph& graphToUse,
                 Backend backend,
                 Vector<int> attributeVaryings = {})
        : printer {graphToUse, backend, locals, std::move(attributeVaryings)}
        , visited(graphToUse.nodeCount())
    {
        locals.resize(graphToUse.nodeCount(), -1);
    }

    const ShaderGraph& graph() const { return printer.graph; }

    // The locals a standalone run of expressions needs - a stage's outputs,
    // which no statement follows - counted over just those expressions.
    std::string defineFor(const Vector<int>& roots, const std::string& indent)
    {
        auto open = Vector<int> {};
        return define(roots, indent, countUsesOver(roots), open);
    }

    // The constant arrays a stage subscripts, declared at the top of its
    // function, before any name has been handed out - so an element may read a
    // uniform or a varying but not a mutable local.
    //
    // The GLSL form drops the const: an element read from a uniform is not a
    // constant expression, which is all GLSL lets one initialise a const with.
    //
    // Emitted in slot order, so an array whose elements read another one finds
    // it already there.
    std::string declareArrays(const Vector<int>& roots, const std::string& indent)
    {
        const auto& arrays = graph().arrays();

        if (arrays.empty())
            return {};

        auto used = Vector<char> {};
        used.resize(arrays.size(), 0);
        auto seen = Vector<char> {};
        seen.resize(graph().nodeCount(), 0);

        for (auto root: roots)
            collectArrays(graph(), root, used, seen);

        auto source = std::string {};

        for (auto slot = 0; slot < arrays.size(); ++slot)
        {
            if (used[slot] == 0)
                continue;

            const auto& array = arrays[slot];

            auto qualifier =
                std::string(printer.backend == Backend::Vulkan ? "" : "const ");

            source += indent + qualifier
                      + typeName(printer.backend, array.elementType) + " a"
                      + std::to_string(slot) + "["
                      + std::to_string(array.elements.size()) + "] = {";

            for (auto i = 0; i < array.elements.size(); ++i)
            {
                if (i > 0)
                    source += ", ";

                source += printer.ref(array.elements[i]);
            }

            source += "};\n";
        }

        return source;
    }

    std::string emitBlock(int block, const std::string& indent)
    {
        auto uses = blockUses(block);
        auto open = Vector<int> {};
        auto source = std::string {};

        for (auto index: graph().block(block).statements)
            source += emitStatement(graph().statement(index), indent, uses, open);

        retire(open);
        return source;
    }

    std::string emitStatement(const Statement& statement,
                              const std::string& indent,
                              const Vector<int>& uses,
                              Vector<int>& open)
    {
        auto inner = indent + "    ";

        if (statement.kind == StatementKind::Loop)
        {
            dropStale(statement, open);

            return indent + "while (" + printer.ref(statement.value) + ")\n" + indent
                   + "{\n" + emitBlock(statement.body, inner) + indent + "}\n";
        }

        auto source = std::string {};

        switch (statement.kind)
        {
            case StatementKind::Declare:
            case StatementKind::Assign:
            {
                auto declares = statement.kind == StatementKind::Declare;
                auto type =
                    declares
                        ? std::string(typeName(printer.backend,
                                               graph().variables()[statement.slot]))
                              + " "
                        : std::string {};

                source = define({statement.value}, indent, uses, open);
                source += indent + type + "v" + std::to_string(statement.slot)
                          + " = " + printer.ref(statement.value) + ";\n";
                break;
            }

            // The condition is evaluated before either body runs, so it is
            // printed while every name still stands; the ones a body moves on
            // from are given up between it and them. An assignment needs no
            // such pass first: its right-hand side is what the variable held
            // before it, which is what the open names still stand for.
            case StatementKind::If:
            {
                source = define({statement.value}, indent, uses, open);

                auto condition = printer.ref(statement.value);
                dropStale(statement, open);

                source += indent + "if (" + condition + ")\n" + indent + "{\n"
                          + emitBlock(statement.body, inner) + indent + "}\n";

                if (statement.elseBody >= 0)
                    source += indent + "else\n" + indent + "{\n"
                              + emitBlock(statement.elseBody, inner) + indent
                              + "}\n";

                break;
            }

            case StatementKind::Break:
                source = indent + "break;\n";
                break;

            case StatementKind::Continue:
                source = indent + "continue;\n";
                break;

            case StatementKind::Store:
            {
                source =
                    define({statement.index, statement.value}, indent, uses, open);
                source += holdTheRecord(statement, indent, open);

                auto element = "buffer" + std::to_string(statement.slot) + "["
                               + printer.ref(statement.index) + "]";
                auto stored = printer.ref(statement.value);

                // An atomic buffer's element is an atomic_uint on Metal and has
                // to be stored through rather than assigned. HLSL's UAV element
                // is an ordinary uint, so the plain assignment is already right
                // there.
                auto atomic =
                    graph().storageBuffers()[statement.slot] == BufferAccess::Atomic;

                if (atomic && printer.backend == Backend::Metal)
                    source += indent + "atomic_store_explicit(&" + element + ", "
                              + stored + ", memory_order_relaxed);\n";
                else
                    source += indent + element + " = " + stored + ";\n";

                break;
            }

            case StatementKind::AtomicAdd:
            {
                source =
                    define({statement.index, statement.value}, indent, uses, open);

                auto name = "v" + std::to_string(statement.slot);
                auto element = "buffer" + std::to_string(statement.bufferSlot) + "["
                               + printer.ref(statement.index) + "]";
                auto addend = printer.ref(statement.value);

                if (printer.backend == Backend::Metal)
                {
                    source += indent + "uint " + name
                              + " = atomic_fetch_add_explicit(&" + element + ", "
                              + addend + ", memory_order_relaxed);\n";
                    break;
                }

                // GLSL's atomicAdd returns the old value, like MSL's.
                if (printer.backend == Backend::Vulkan)
                {
                    source += indent + "uint " + name + " = atomicAdd(" + element
                              + ", " + addend + ");\n";
                    break;
                }

                // Two lines here rather than one: InterlockedAdd hands the old
                // value back through an out parameter, so the name has to exist
                // before the call that fills it.
                source += indent + "uint " + name + ";\n";
                source += indent + "InterlockedAdd(" + element + ", " + addend + ", "
                          + name + ");\n";
                break;
            }

            case StatementKind::SharedStore:
                source =
                    define({statement.index, statement.value}, indent, uses, open);
                source += indent + "s" + std::to_string(statement.slot) + "["
                          + printer.ref(statement.index)
                          + "] = " + printer.ref(statement.value) + ";\n";
                break;

            // The synchronisation point itself. The names it invalidates -
            // anything computed from shared memory - are given up by the
            // dropStale below, the same pass an assignment retires its
            // variable's readers through.
            // GLSL says it in two calls: the memory barrier publishes what
            // was written, the execution barrier is where the group meets.
            case StatementKind::Barrier:
                source = barrierStatement(printer.backend, indent);
                break;

            // Several statements on every backend, laid down where the
            // reduction was written so the barriers inside it keep their place
            // among the stores around them.
            case StatementKind::GroupReduce:
                source = define({statement.value}, indent, uses, open);
                source += groupReduction(statement, indent);
                break;

            // The SIMD-group matrix statements, each one intrinsic on Metal and
            // a loop over the 64 elements of a per-thread copy everywhere else.
            case StatementKind::SimdMatrixFill:
                source = define({statement.value}, indent, uses, open);
                source += simdMatrixFill(statement, indent);
                break;

            case StatementKind::SimdMatrixLoad:
            case StatementKind::SimdMatrixStore:
                source =
                    define({statement.index, statement.stride}, indent, uses, open);
                source += simdMatrixTransfer(statement, indent);
                break;

            case StatementKind::SimdMatrixMultiplyAdd:
                source = simdMatrixMultiplyAdd(statement, indent);
                break;

            // GLSL's imageStore takes a *signed* coordinate; MSL takes the
            // colour first, HLSL subscripts the texture like an array.
            case StatementKind::TextureStore:
            {
                source = define({statement.index, statement.indexY, statement.value},
                                indent,
                                uses,
                                open);

                auto name = "texture" + std::to_string(statement.slot);
                auto pair = std::string(
                    printer.backend == Backend::Vulkan ? "ivec2(" : "uint2(");
                auto coordinates = pair + printer.ref(statement.index) + ", "
                                   + printer.ref(statement.indexY) + ")";
                auto color = printer.ref(statement.value);

                if (printer.backend == Backend::Metal)
                    source += indent + name + ".write(" + color + ", " + coordinates
                              + ");\n";
                else if (printer.backend == Backend::Vulkan)
                    source += indent + "imageStore(" + name + ", " + coordinates
                              + ", " + color + ");\n";
                else
                    source +=
                        indent + name + "[" + coordinates + "] = " + color + ";\n";

                break;
            }

            case StatementKind::Loop:
                break;
        }

        // Afterwards either way, for the names this statement's own expressions
        // introduced: a value read out of the variable it then wrote.
        dropStale(statement, open);
        return source;
    }

private:
    // MSL folds within each SIMD group first and combines the few partials
    // through the scratch; HLSL under FXC has no wave intrinsic and GLSL is
    // held to what lavapipe compiles with no extension, so both take the
    // scratch tree. Either way the result lands in the reduction's variable on
    // every thread, and a trailing barrier leaves the scratch free for the
    // next one.
    std::string groupReduction(const Statement& statement, const std::string& indent)
    {
        auto elementType = graph().variables()[statement.slot];
        auto type = std::string(typeName(printer.backend, elementType));
        auto name = "v" + std::to_string(statement.slot);
        auto scratch = std::string(groupScratchName(elementType));
        auto step = "gr" + std::to_string(statement.slot);
        auto contributed = printer.ref(statement.value);
        auto barrier = barrierStatement(printer.backend, indent);

        if (printer.backend == Backend::Metal)
        {
            auto source = indent + type + " " + name + " = "
                          + metalSimdReduction(statement.reduction) + "("
                          + contributed + ");\n";

            source += indent + "if (simdLane == 0u)\n" + indent + "    " + scratch
                      + "[simdIndex] = " + name + ";\n";
            source += barrier;
            source += indent + name + " = " + scratch + "[0];\n";
            source += indent + "for (uint " + step + " = 1u; " + step
                      + " < simdCount; ++" + step + ")\n";
            source +=
                indent + "    " + name + " = "
                + foldedPair(statement.reduction, name, scratch + "[" + step + "]")
                + ";\n";

            return source + barrier;
        }

        auto lane = std::string(groupLaneName(printer.backend));
        auto threads = threadsPerGroup(graph());
        auto element = scratch + "[" + lane + "]";
        auto partner = scratch + "[" + lane + " + " + step + "]";

        auto source = indent + element + " = " + contributed + ";\n";
        source += barrier;
        source += indent + "for (uint " + step + " = "
                  + std::to_string(reductionStride(threads)) + "u; " + step
                  + " > 0u; " + step + " >>= 1u)\n" + indent + "{\n";
        source += indent + "    if (" + lane + " < " + step + " && " + lane + " + "
                  + step + " < " + std::to_string(threads) + "u)\n";
        source += indent + "        " + element + " = "
                  + foldedPair(statement.reduction, element, partner) + ";\n";
        source += barrierStatement(printer.backend, indent + "    ");
        source += indent + "}\n";
        source += indent + type + " " + name + " = " + scratch + "[0];\n";

        return source + barrier;
    }

    bool metal() const { return printer.backend == Backend::Metal; }

    // The four matrix statements below. An 8x8 fragment on Metal is a type MSL
    // has, and each operation on one is a single intrinsic; on the two backends
    // with no wave matrix operation it is 64 floats of every thread's own,
    // computed redundantly by all simdGroupWidth lanes of what would have been
    // one SIMD group, so that no lane needs a value another lane holds.
    //
    // Correctness at whatever it costs, as the reduction's tree is where there
    // is no wave intrinsic - and unlike that tree, this is not the fastest form
    // the hardware would allow, which is why the README says so.
    std::string simdMatrixDeclaration(int slot) const
    {
        if (metal())
            return "simdgroup_float8x8 " + simdMatrixName(slot);

        return "float " + simdMatrixName(slot) + "["
               + std::to_string(simdMatrixSize * simdMatrixSize) + "]";
    }

    std::string simdMatrixFill(const Statement& statement, const std::string& indent)
    {
        auto name = simdMatrixName(statement.slot);
        auto value = printer.ref(statement.value);

        if (metal())
            return indent + simdMatrixDeclaration(statement.slot)
                   + " = make_filled_simdgroup_matrix<float, "
                   + std::to_string(simdMatrixSize) + ", "
                   + std::to_string(simdMatrixSize) + ">(" + value + ");\n";

        auto step = name + "e";
        auto elements = std::to_string(simdMatrixSize * simdMatrixSize);

        auto source = indent + simdMatrixDeclaration(statement.slot) + ";\n";
        source += indent + "for (uint " + step + " = 0u; " + step + " < " + elements
                  + "u; ++" + step + ")\n";
        source += indent + "    " + name + "[" + step + "] = " + value + ";\n";
        return source;
    }

    // The load and the store are the same patch walked in the two directions,
    // so they are one function: what changes is which side of the assignment
    // each is on, and that only the store needs a lane to make it.
    std::string simdMatrixTransfer(const Statement& statement, std::string indent)
    {
        auto loading = statement.kind == StatementKind::SimdMatrixLoad;
        auto name = simdMatrixName(statement.slot);
        auto memory = simdMatrixMemoryName(statement);
        auto offset = bracketed(printer.ref(statement.index));
        auto stride = bracketed(printer.ref(statement.stride));

        if (metal())
        {
            auto pointer = memory + " + " + offset;

            if (loading)
                return indent + simdMatrixDeclaration(statement.slot) + ";\n"
                       + indent + "simdgroup_load(" + name + ", " + pointer + ", "
                       + stride + ");\n";

            return indent + "simdgroup_store(" + name + ", " + pointer + ", "
                   + stride + ");\n";
        }

        auto row = name + "r";
        auto column = name + "c";
        auto side = std::to_string(simdMatrixSize);
        auto element = memory + "[" + offset + " + " + row + " * " + stride + " + "
                       + column + "]";
        auto held = name + "[" + row + " * " + side + "u + " + column + "]";

        auto source = std::string {};

        if (loading)
            source += indent + simdMatrixDeclaration(statement.slot) + ";\n";
        else
        {
            source += indent + "if (" + simdLeadLane(printer.backend) + ")\n";
            source += indent + "{\n";
            indent += "    ";
        }

        source += indent + "for (uint " + row + " = 0u; " + row + " < " + side
                  + "u; ++" + row + ")\n";
        source += indent + "    for (uint " + column + " = 0u; " + column + " < "
                  + side + "u; ++" + column + ")\n";
        source += indent + "        "
                  + (loading ? held + " = " + element : element + " = " + held)
                  + ";\n";

        if (!loading)
        {
            indent.resize(indent.size() - 4);
            source += indent + "}\n";
        }

        return source;
    }

    std::string simdMatrixMultiplyAdd(const Statement& statement,
                                      const std::string& indent)
    {
        auto accumulator = simdMatrixName(statement.slot);
        auto left = simdMatrixName(statement.left);
        auto right = simdMatrixName(statement.right);

        if (metal())
            return indent + "simdgroup_multiply_accumulate(" + accumulator + ", "
                   + left + ", " + right + ", " + accumulator + ");\n";

        // Through a temporary, and inside a block of its own: an accumulator is
        // allowed to be one of the operands, and a second product in the same
        // scope would otherwise redeclare the temporary.
        auto sum = accumulator + "p";
        auto row = accumulator + "i";
        auto column = accumulator + "j";
        auto step = accumulator + "k";
        auto copy = accumulator + "n";
        auto side = std::to_string(simdMatrixSize);
        auto elements = std::to_string(simdMatrixSize * simdMatrixSize);

        auto source = indent + "{\n";
        source += indent + "    float " + sum + "[" + elements + "];\n";
        source += indent + "    for (uint " + row + " = 0u; " + row + " < " + side
                  + "u; ++" + row + ")\n";
        source += indent + "        for (uint " + column + " = 0u; " + column + " < "
                  + side + "u; ++" + column + ")\n";
        source += indent + "        {\n";
        source += indent + "            float " + sum + "e = 0.0;\n";
        source += indent + "            for (uint " + step + " = 0u; " + step + " < "
                  + side + "u; ++" + step + ")\n";
        source += indent + "                " + sum + "e += " + left + "[" + row
                  + " * " + side + "u + " + step + "] * " + right + "[" + step
                  + " * " + side + "u + " + column + "];\n";
        source += indent + "            " + sum + "[" + row + " * " + side + "u + "
                  + column + "] = " + accumulator + "[" + row + " * " + side + "u + "
                  + column + "] + " + sum + "e;\n";
        source += indent + "        }\n";
        source += indent + "    for (uint " + copy + " = 0u; " + copy + " < "
                  + elements + "u; ++" + copy + ")\n";
        source += indent + "        " + accumulator + "[" + copy + "] = " + sum + "["
                  + copy + "];\n";
        source += indent + "}\n";
        return source;
    }

    Vector<int> countUsesOver(const Vector<int>& roots) const
    {
        auto count = graph().nodeCount();
        auto uses = Vector<int> {};
        auto seen = Vector<char> {};

        uses.resize(count, 0);
        seen.resize(count, 0);

        for (auto root: roots)
            countUses(graph(), root, uses, seen);

        return uses;
    }

    // How often the statements of one block reach each node, its nested bodies
    // counted in: a name is handed out only where the statement being emitted
    // evaluates the node anyway, so a body's use of one costs that body nothing.
    Vector<int> blockUses(int block) const
    {
        auto roots = Vector<int> {};
        collectUseRoots(graph(), block, roots);
        return countUsesOver(roots);
    }

    std::string define(const Vector<int>& roots,
                       const std::string& indent,
                       const Vector<int>& uses,
                       Vector<int>& open)
    {
        auto count = graph().nodeCount();
        auto ordered = Vector<char> {};
        ordered.resize(count, 0);
        auto order = Vector<int> {};

        for (auto root: roots)
            orderLocals(graph(), root, uses, locals, ordered, order);

        auto source = std::string {};

        for (auto node: order)
            source += bind(node, indent, open);

        return source;
    }

    std::string bind(int node, const std::string& indent, Vector<int>& open)
    {
        locals[node] = localCount++;
        open.add(node);

        return indent
               + std::string(typeName(printer.backend, graph().expr(node).type))
               + " t" + std::to_string(locals[node]) + " = " + printer.print(node)
               + ";\n";
    }

    std::string holdTheRecord(const Statement& statement,
                              const std::string& indent,
                              Vector<int>& open)
    {
        if (statement.recordComponentsLeft <= 0 || locals[statement.record] >= 0
            || !readsSlot(statement.record, statement.slot))
            return {};

        return bind(statement.record, indent, open);
    }

    bool readsSlot(int node, int slot)
    {
        written.assign(graph().variables().size(), 0);
        buffersWritten.assign(graph().storageBuffers().size(), 0);
        buffersWritten[slot] = 1;
        visited.restart();

        return readsStale(graph(), node, written, buffersWritten, false, visited);
    }

    void retire(Vector<int>& open)
    {
        for (auto node: open)
            locals[node] = -1;

        open.clear();
    }

    void dropStale(const Statement& statement, Vector<int>& open)
    {
        if (open.empty())
            return;

        auto sharedMoved = touchesShared(graph(), statement);

        written.assign(graph().variables().size(), 0);
        collectWrites(graph(), statement, written);

        buffersWritten.assign(graph().storageBuffers().size(), 0);
        collectBufferWrites(graph(), statement, buffersWritten);

        // A statement that leaves no variable and no buffer holding something
        // else and moves no shared memory cannot have staled a name, and most
        // do not: a break, a continue, and an if whose bodies only compute.
        // Asking each open name about an empty set is the same walk for a
        // guaranteed no.
        if (!written.contains(1) && !buffersWritten.contains(1) && !sharedMoved)
            return;

        auto heldRecord = statement.recordComponentsLeft > 0 ? statement.record : -1;

        auto kept = Vector<int> {};

        for (auto node: open)
        {
            visited.restart();

            if (node != heldRecord
                && readsStale(
                    graph(), node, written, buffersWritten, sharedMoved, visited))
                locals[node] = -1;
            else
                kept.add(node);
        }

        open = std::move(kept);
    }

public:
    Vector<int> locals; // node id -> local index, -1 = inline
    ExprPrinter printer;
    int localCount = 0;

private:
    // Held by the emitter rather than by the walk, so that naming a stage costs
    // one buffer instead of one per name per statement.
    VisitSet visited;
    Vector<char> written;
    Vector<char> buffersWritten;
};

// Whether the expression tree under node reads a uniform. A Varying read is the
// fragment-stage boundary: its vertex-stage source tree is walked separately as
// part of the vertex stage, so the walk stops there.
//
// The visited set is not an optimisation here, it is what makes the walk
// finite in practice. What this walks is a graph rather than a tree - the
// emitter's whole reason for existing is that a shared subtree is stored once -
// and a walk that revisits a shared node once per path through it is
// exponential in the sharing, not quadratic. It went unnoticed for as long as
// every shader was small enough that the exponent did not matter.
//
// A node reached twice is a node whose answer is already in the result: either
// the walk that reached it first found a uniform, in which case it returned
// true and this one is unreachable, or it did not, in which case there is none
// under there to find.
bool referencesUniform(const ShaderGraph& graph, int node, VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return false;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::Uniform)
        return true;

    if (expr.kind == ExprKind::Varying)
        return false;

    for (auto argument: expr.args)
        if (referencesUniform(graph, argument, seen))
            return true;

    return false;
}

// Every expression a block's statements evaluate, gathered so a stage sees what
// its statements read and not only what its output expression does. Without
// this a uniform read only from inside a loop would go undeclared: the
// expression walk starts at the fragment colour and never reaches it.
void collectStatementRoots(const ShaderGraph& graph, int block, Vector<int>& roots)
{
    for (auto index: graph.block(block).statements)
    {
        const auto& statement = graph.statement(index);
        roots.add(statement.value);
        roots.add(statement.index);
        roots.add(statement.indexY);
        roots.add(statement.stride);

        if (statement.body >= 0)
            collectStatementRoots(graph, statement.body, roots);

        if (statement.elseBody >= 0)
            collectStatementRoots(graph, statement.elseBody, roots);
    }
}

// One visited set across every root, not one per root: a stage's roots share
// most of their graph, and a node already known to hold no uniform holds none
// whichever root reached it.
bool anyReferencesUniform(const ShaderGraph& graph, const Vector<int>& roots)
{
    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: roots)
        if (referencesUniform(graph, root, seen))
            return true;

    return false;
}

// Every expression the vertex stage evaluates: the clip position and each
// varying it hands the fragment stage.
Vector<int> vertexStageRoots(const ShaderGraph& graph)
{
    auto roots = Vector<int> {};
    roots.add(graph.position());

    for (const auto& varying: graph.varyings())
        roots.add(varying.sourceNode);

    return roots;
}

// Its fragment sibling: the colour, the alpha test when there is one, and what
// the statements evaluate. Wider than the roots the colour's locals are planned
// from, which is why emit() keeps both - a value a statement reads is declared
// by the stage but named where the statement is emitted.
Vector<int> fragmentStageRoots(const ShaderGraph& graph)
{
    auto roots = Vector<int> {graph.fragment()};

    if (graph.discard() >= 0)
        roots.add(graph.discard());

    collectStatementRoots(graph, ShaderGraph::rootBlock, roots);
    return roots;
}

// A Varying is the stage boundary; an array element is followed, the array
// being declared inside the stage that subscripts it.
void collectAttributeReads(const ShaderGraph& graph,
                           int node,
                           Vector<char>& reached,
                           VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::Varying)
        return;

    if (expr.kind == ExprKind::Input)
        reached[node] = 1;

    if (expr.kind == ExprKind::ArrayRead)
        for (auto element: graph.arrays()[expr.index].elements)
            collectAttributeReads(graph, element, reached, seen);

    for (auto argument: expr.args)
        collectAttributeReads(graph, argument, reached, seen);
}

// -1 when no declared varying carries `source`.
int varyingCarrying(const ShaderGraph& graph, int source)
{
    for (auto i = 0; i < graph.varyings().size(); ++i)
        if (graph.varyings()[i].sourceNode == source)
            return i;

    return -1;
}

// A fragment-stage read of a vertex attribute names an identifier no dialect
// gives that stage, so each is promoted to a varying after the declared ones.
struct PromotedAttributes
{
    Vector<int> varyingOf; // attribute slot -> varying index, -1 = not read
    Vector<int> carried; // implicit varying order -> attribute slot
};

PromotedAttributes promotedAttributes(const ShaderGraph& graph)
{
    auto promoted = PromotedAttributes {};
    promoted.varyingOf.resize(graph.inputs().size(), -1);

    auto reached = Vector<char> {};
    reached.resize(graph.nodeCount(), 0);

    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: fragmentStageRoots(graph))
        collectAttributeReads(graph, root, reached, seen);

    // Ascending node order, so the implicit varyings follow declaration order.
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        if (reached[node] == 0)
            continue;

        auto slot = graph.expr(node).index;
        auto carrier = varyingCarrying(graph, node);

        if (carrier < 0)
        {
            carrier = graph.varyings().size() + promoted.carried.size();
            promoted.carried.add(slot);
        }

        promoted.varyingOf[slot] = carrier;
    }

    return promoted;
}

struct StageVarying
{
    ValueType type = ValueType::Float;
    int sourceNode = -1;
    int attribute = -1;
};

Vector<StageVarying> stageVaryings(const ShaderGraph& graph,
                                   const PromotedAttributes& promoted)
{
    auto slots = Vector<StageVarying> {};

    for (const auto& varying: graph.varyings())
        slots.add(StageVarying {varying.type, varying.sourceNode, -1});

    for (auto attribute: promoted.carried)
        slots.add(StageVarying {graph.inputs()[attribute], -1, attribute});

    return slots;
}

// How a buffer slot spells itself: the access decides the qualifier, the
// element type what is qualified. An atomic slot answers to neither - its
// elements are the type each language requires an interlocked operation to act
// through.
const char* metalBufferType(BufferAccess access, ValueType elementType)
{
    auto integers = elementType == ValueType::UInt;

    switch (access)
    {
        case BufferAccess::Read:
            return integers ? "device const uint*" : "device const float*";
        case BufferAccess::Write:
            return integers ? "device uint*" : "device float*";
        case BufferAccess::Atomic:
            return "device atomic_uint*";
    }

    return "device const float*";
}

const char* hlslBufferType(BufferAccess access, ValueType elementType)
{
    auto integers = elementType == ValueType::UInt;

    switch (access)
    {
        case BufferAccess::Read:
            return integers ? "StructuredBuffer<uint>" : "StructuredBuffer<float>";
        case BufferAccess::Write:
            return integers ? "RWStructuredBuffer<uint>"
                            : "RWStructuredBuffer<float>";
        case BufferAccess::Atomic:
            return "RWStructuredBuffer<uint>";
    }

    return "StructuredBuffer<float>";
}

// Which storage-buffer slots a run of expressions subscripts. A render stage
// declares only the buffers it reads - unlike a kernel, where every slot is a
// parameter of the one entry point - so the vertex and fragment functions each
// need their own answer.
void collectBufferSlots(const ShaderGraph& graph,
                        int node,
                        Vector<char>& used,
                        VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::BufferRead && expr.index < used.size())
        used[expr.index] = 1;

    for (auto argument: expr.args)
        collectBufferSlots(graph, argument, used, seen);
}

Vector<char> bufferSlotsUsedBy(const ShaderGraph& graph, const Vector<int>& roots)
{
    auto used = Vector<char> {};
    used.resize(graph.storageBuffers().size(), 0);

    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: roots)
        collectBufferSlots(graph, root, used, seen);

    return used;
}

// The MSL parameters for the storage buffers a render stage reads, always read
// only: a vertex or fragment function has no writable buffer here, which is the
// whole of what separates this from the kernel signature above.
std::string bufferParameters(const ShaderGraph& graph, const Vector<int>& roots)
{
    auto used = bufferSlotsUsedBy(graph, roots);
    auto source = std::string {};

    for (auto i = 0; i < used.size(); ++i)
        if (used[i] != 0)
            source += ",\n    "
                      + std::string(metalBufferType(BufferAccess::Read,
                                                    graph.storageElementType(i)))
                      + " buffer" + std::to_string(i) + " [[buffer("
                      + std::to_string(RenderPass::bufferBase + i) + ")]]";

    return source;
}

// The Uniforms struct shared by both stages (and the HLSL cbuffer wrapping it).
// The CPU block is packed with MSL struct alignment (UniformLayout.h); HLSL
// cbuffer packing only forbids straddling a 16-byte register, so a vector after
// a scalar would land lower than the CPU wrote it - explicit pad scalars are
// emitted wherever the two rule sets disagree.
//
// std140 disagrees the other way: a vec3 is twelve bytes there, so a scalar
// after one needs a pad too.
std::string uniformBlock(Backend backend,
                         const Vector<ValueType>& types,
                         const Vector<std::string>& names,
                         int binding = vulkanUniformBinding)
{
    auto glsl = backend == Backend::Vulkan;

    auto source = glsl ? "layout(std140, set = 0, binding = "
                             + std::to_string(binding) + ") uniform Uniforms\n{\n"
                       : std::string {"struct Uniforms\n{\n"};

    auto offsets = uniformOffsets(types);
    auto cursor = 0;
    auto padCount = 0;

    for (auto i = 0; i < types.size(); ++i)
    {
        auto type = types[i];

        if (backend != Backend::Metal)
        {
            auto packedOffset = [&](int at)
            {
                return glsl ? std140PackedOffset(at, type)
                            : hlslPackedOffset(at, type);
            };

            while (packedOffset(cursor) < offsets[i])
            {
                source += "    float pad" + std::to_string(padCount++) + ";\n";
                cursor += 4;
            }

            cursor = offsets[i] + byteSize(type);
        }

        source +=
            "    " + std::string(typeName(backend, type)) + " " + names[i] + ";\n";
    }

    // The instance name is what keeps uniforms.uN the one spelling in all three.
    source += glsl ? "} uniforms;\n\n" : "};\n\n";

    if (backend == Backend::DirectX)
        source += "cbuffer UniformsCB : register(b0)\n{\n"
                  "    Uniforms uniforms;\n};\n\n";

    return source;
}

// A GLSL storage block is read-write by default; a read one says so.
const char* glslBufferQualifier(BufferAccess access)
{
    return access == BufferAccess::Read ? "readonly " : "";
}

// The element type on the same terms metalBufferType and hlslBufferType take
// it: an atomic slot is unsigned integers whatever it was declared with, since
// atomicAdd acts through nothing else.
const char* glslBufferElement(BufferAccess access, ValueType elementType)
{
    if (access == BufferAccess::Atomic || elementType == ValueType::UInt)
        return "uint";

    return "float";
}

// The instance name is left off so the run of elements is a global named
// buffer<slot>, which prints byte-identically to the other two dialects.
std::string glslBufferBlock(BufferAccess access,
                            ValueType elementType,
                            int slot,
                            int binding)
{
    auto index = std::to_string(slot);

    return "layout(std430, set = 0, binding = " + std::to_string(binding) + ") "
           + glslBufferQualifier(access) + "buffer Buffer" + index + "\n{\n    "
           + glslBufferElement(access, elementType) + " buffer" + index
           + "[];\n};\n";
}

// What a sampled texture slot is declared as, which is the whole of what a cube
// changes: every declaration goes through these, so no two can disagree.
//
// GLSL is the exception: its sampler2D over a depth image returns four
// channels, so ExprKind::Sample takes the .r there.
const char* metalTextureType(TextureKind kind)
{
    switch (kind)
    {
        case TextureKind::Cube:
            return "texturecube<float>";
        case TextureKind::Depth2D:
            return "depth2d<float>";
        default:
            return "texture2d<float>";
    }
}

// `Texture2D<float>` rather than the bare `Texture2D` a colour slot gets: the
// SRV over the depth resource is a single-channel R32_FLOAT view (see
// depthShaderResourceFormat), and the typed declaration is what makes Sample
// return the one float MSL's depth2d does.
const char* hlslTextureType(TextureKind kind)
{
    switch (kind)
    {
        case TextureKind::Cube:
            return "TextureCube";
        case TextureKind::Depth2D:
            return "Texture2D<float>";
        default:
            return "Texture2D";
    }
}

// A combined image sampler: the pipeline layout supplies an immutable sampler
// per binding, so the sampling configuration never reaches the source.
const char* glslTextureType(TextureKind kind)
{
    return kind == TextureKind::Cube ? "samplerCube" : "sampler2D";
}

// imageStore is all a kernel does with one, so the image needs no format layout
// qualifier - which keeps it agnostic of the format it was created in.
std::string glslWritableTexture(int slot, int binding)
{
    return "layout(set = 0, binding = " + std::to_string(binding)
           + ") uniform writeonly image2D texture" + std::to_string(slot) + ";\n";
}

std::string glslSampledTexture(TextureKind kind, int slot, int binding)
{
    return "layout(set = 0, binding = " + std::to_string(binding) + ") uniform "
           + glslTextureType(kind) + " texture" + std::to_string(slot) + ";\n";
}

// The SamplerState globals an HLSL stage needs: one per sampling configuration
// any of its readable textures asked for, at the register the root signature
// put that configuration's static sampler on.
//
// Declaring them per configuration rather than per texture is what stops the
// sampler registers from capping maxTextureSlots. HLSL has 16 of them
// (s0..s15), so one sampler per (slot, configuration) pair - which is what this
// emitted until a shader needed a fifth texture - ran out at four slots. Metal
// never had the question, because MSL passes a sampler as a function argument
// rather than binding it to a register, so the Metal path below still declares
// one per texture.
//
// A write-access texture is skipped for the same reason it is declared as an
// RWTexture2D: there is nothing to sample it with.
std::string hlslSamplerDeclarations(const ShaderGraph& graph)
{
    bool used[samplingConfigurations] = {};

    for (auto i = 0; i < graph.textureCount(); ++i)
        if (graph.textureAccess(i) != TextureAccess::Write)
            used[samplingIndex(graph.textureSampling(i))] = true;

    auto source = std::string {};

    for (auto configuration = 0; configuration < samplingConfigurations;
         ++configuration)
        if (used[configuration])
            source += "SamplerState samplerConfig" + std::to_string(configuration)
                      + " : register(s" + std::to_string(configuration) + ");\n";

    return source;
}

// The early return the rounded-up dispatch needs, over as many extents as the
// rank has.
std::string boundsGuard(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "    if (gid >= uniforms.count)\n        return;\n";

    auto condition = std::string {};

    for (auto i = 0; i < gridExtentCount(rank); ++i)
        condition += (i == 0 ? "" : " || ")
                     + ("gid" + std::string(componentSuffix(i))) + " >= uniforms."
                     + gridExtentName(i);

    return "    if (" + condition + ")\n        return;\n";
}

// Compute kernel emission. The expression printer is the render one; only the
// scaffolding differs: storage buffers and the uniform block are MSL kernel
// parameters but HLSL globals, and the work-item id arrives as a builtin
// parameter on Metal and as SV_DispatchThreadID on D3D. The block always ends
// with the implicit grid extents the bounds guard reads - one count for a 1D
// kernel, a width and a height for a 2D one, a depth as well for a 3D one - and
// the kernel opens with the guard the rounded-up dispatch needs; ComputeProgram
// appends the matching CPU values.
//
// GLSL takes the HLSL shape, with gl_GlobalInvocationID for the id and no stage
// macro: a kernel has one entry point, so its main() is unguarded.
std::string emitCompute(const ShaderGraph& graph, Backend backend)
{
    auto source = std::string {};
    auto rank = graph.dispatchRank();

    assert((!graph.usesSimdGroups()
            || graph.threadGroupShape().threadCount() % simdGroupWidth == 0)
           && "eacp: a kernel using SIMD groups has to be dispatched in a "
              "threadgroup of a whole number of them - a multiple of "
              "ComputeProgram::simdWidth threads. A group that is not leaves a "
              "partial SIMD group, whose matrix operations are undefined.");

    if (backend == Backend::Metal)
        source += "#include <metal_stdlib>\nusing namespace metal;\n\n";

    if (backend == Backend::Vulkan)
        source += "#version 450\n\n";

    source += helperDefinitions(graph, backend);

    auto uniformTypes = graph.uniforms();
    auto uniformNames = Vector<std::string> {};

    for (auto i = 0; i < uniformTypes.size(); ++i)
        uniformNames.add("u" + std::to_string(i));

    if (rank == DispatchRank::OneD)
    {
        uniformTypes.add(ValueType::UInt);
        uniformNames.add("count");
    }
    else
    {
        for (auto i = 0; i < gridExtentCount(rank); ++i)
        {
            uniformTypes.add(ValueType::UInt);
            uniformNames.add(gridExtentName(i));
        }
    }

    source += uniformBlock(
        backend, uniformTypes, uniformNames, vulkanComputeUniformBinding);

    const auto& buffers = graph.storageBuffers();

    assert(buffers.size() <= ComputePass::maxBufferSlots
           && "eacp: a kernel may bind ComputePass::maxBufferSlots storage "
              "buffers. A slot past that has no register the root signature "
              "declares, so it binds nowhere and reads zeroes.");

    if (backend == Backend::Metal)
    {
        source += "kernel void computeMain(";

        for (auto i = 0; i < buffers.size(); ++i)
        {
            source +=
                std::string(metalBufferType(buffers[i], graph.storageElementType(i)))
                + " buffer" + std::to_string(i) + " [[buffer(" + std::to_string(i)
                + ")]],\n    ";
        }

        // Textures are kernel parameters like the buffers, on an index space of
        // their own. A written one takes the write access qualifier and no
        // sampler: there is nothing to sample it with and nothing to read.
        for (auto i = 0; i < graph.textureCount(); ++i)
        {
            auto slot = std::to_string(i);

            if (graph.textureAccess(i) == TextureAccess::Write)
            {
                source += "texture2d<float, access::write> texture" + slot
                          + " [[texture(" + slot + ")]],\n    ";
                continue;
            }

            source += std::string(metalTextureType(graph.textureKind(i)))
                      + " texture" + slot + " [[texture(" + slot
                      + ")]],\n    sampler sampler" + slot + " [[sampler(" + slot
                      + ")]],\n    ";
        }

        source += "constant Uniforms& uniforms [[buffer("
                  + std::to_string(ComputePass::uniformBase) + ")]],\n    ";
        source +=
            std::string(indexTypeName(rank)) + " gid [[thread_position_in_grid]]";

        auto indexType = std::string(indexTypeName(rank));

        if (graph.usesLocalId())
            source +=
                ",\n    " + indexType + " lid [[thread_position_in_threadgroup]]";

        if (graph.usesGroupId())
            source +=
                ",\n    " + indexType + " tgid [[threadgroup_position_in_grid]]";

        // What a group reduction folds through: the SIMD-group intrinsics run
        // per SIMD group, so combining their partials takes the lane, the SIMD
        // group's index and how many of them the threadgroup was given. A
        // kernel holding SIMD-group matrices takes the same three, the index
        // being what places the block of the output each SIMD group owns.
        if (graph.usesGroupReduction() || graph.usesSimdGroups())
            source += ",\n    uint simdLane [[thread_index_in_simdgroup]],\n    "
                      "uint simdIndex [[simdgroup_index_in_threadgroup]],\n    "
                      "uint simdCount [[simdgroups_per_threadgroup]]";

        source += ")\n{\n";

        // Threadgroup arrays are body-scope declarations on Metal, ahead of
        // everything that subscripts them.
        for (auto i = 0; i < graph.sharedArrays().size(); ++i)
        {
            const auto& shared = graph.sharedArrays()[i];

            source += "    threadgroup "
                      + std::string(typeName(backend, shared.elementType)) + " s"
                      + std::to_string(i) + "[" + std::to_string(shared.elements)
                      + "];\n";
        }

        for (auto elementType: graph.groupReductionTypes())
            source += "    threadgroup "
                      + std::string(typeName(backend, elementType)) + " "
                      + groupScratchName(elementType) + "["
                      + std::to_string(threadsPerGroup(graph)) + "];\n";
    }
    else if (backend == Backend::Vulkan)
    {
        // One descriptor set carries the lot, at the Metal indices.
        for (auto i = 0; i < buffers.size(); ++i)
            source += glslBufferBlock(buffers[i],
                                      graph.storageElementType(i),
                                      i,
                                      vulkanComputeBufferBinding(i));

        if (buffers.size() > 0)
            source += "\n";

        for (auto i = 0; i < graph.textureCount(); ++i)
        {
            auto binding = vulkanComputeTextureBinding(i);

            source += graph.textureAccess(i) == TextureAccess::Write
                          ? glslWritableTexture(i, binding)
                          : glslSampledTexture(graph.textureKind(i), i, binding);
        }

        if (graph.textureCount() > 0)
            source += "\n";

        for (auto i = 0; i < graph.sharedArrays().size(); ++i)
        {
            const auto& shared = graph.sharedArrays()[i];

            source += "shared " + std::string(typeName(backend, shared.elementType))
                      + " s" + std::to_string(i) + "["
                      + std::to_string(shared.elements) + "];\n";
        }

        for (auto elementType: graph.groupReductionTypes())
            source += "shared " + std::string(typeName(backend, elementType)) + " "
                      + groupScratchName(elementType) + "["
                      + std::to_string(threadsPerGroup(graph)) + "];\n";

        if (graph.sharedArrays().size() > 0 || graph.usesGroupReduction())
            source += "\n";

        const auto group = graph.threadGroupShape();

        source += "layout(local_size_x = " + std::to_string(group.x)
                  + ", local_size_y = " + std::to_string(group.y)
                  + ", local_size_z = " + std::to_string(group.z) + ") in;\n\n";

        source += "void main()\n{\n";

        // The three builtins are uvec3 whatever the rank, so each index is the
        // swizzle of its own, exactly as the HLSL semantics are below.
        auto indexType = std::string(glslIndexTypeName(rank));
        auto swizzle = std::string(indexSwizzle(rank));

        source +=
            "    " + indexType + " gid = gl_GlobalInvocationID" + swizzle + ";\n";

        if (graph.usesLocalId())
            source +=
                "    " + indexType + " lid = gl_LocalInvocationID" + swizzle + ";\n";

        if (graph.usesGroupId())
            source +=
                "    " + indexType + " tgid = gl_WorkGroupID" + swizzle + ";\n";
    }
    else
    {
        // SRV t<slot> / UAV u<slot> with one shared slot counter, matching the
        // flat Metal indices ComputePass binds both backends with.
        for (auto i = 0; i < buffers.size(); ++i)
        {
            auto slot = std::to_string(i);
            auto readOnly = buffers[i] == BufferAccess::Read;

            source += hlslBufferType(buffers[i], graph.storageElementType(i));
            source += " buffer";
            source += slot;
            source += readOnly ? " : register(t" : " : register(u";
            source += slot + ");\n";
        }

        if (buffers.size() > 0)
            source += "\n";

        // Textures are globals here, and their registers start above every
        // buffer slot's: a texture and a storage buffer share the t and u
        // spaces on this backend, while their slots are counted separately. See
        // ComputePass::textureRegisterBase.
        for (auto i = 0; i < graph.textureCount(); ++i)
        {
            auto slot = std::to_string(i);
            auto reg = std::to_string(ComputePass::textureRegisterBase + i);

            if (graph.textureAccess(i) == TextureAccess::Write)
            {
                source += "RWTexture2D<float4> texture" + slot + " : register(u"
                          + reg + ");\n";
                continue;
            }

            source += std::string(hlslTextureType(graph.textureKind(i))) + " texture"
                      + slot + " : register(t" + reg + ");\n";
        }

        source += hlslSamplerDeclarations(graph);

        if (graph.textureCount() > 0)
            source += "\n";

        // Threadgroup arrays are globals on HLSL, like the buffers above.
        for (auto i = 0; i < graph.sharedArrays().size(); ++i)
        {
            const auto& shared = graph.sharedArrays()[i];

            source += "groupshared " + std::string(typeName(shared.elementType))
                      + " s" + std::to_string(i) + "["
                      + std::to_string(shared.elements) + "];\n";
        }

        for (auto elementType: graph.groupReductionTypes())
            source += "groupshared " + std::string(typeName(elementType)) + " "
                      + groupScratchName(elementType) + "["
                      + std::to_string(threadsPerGroup(graph)) + "];\n";

        if (graph.sharedArrays().size() > 0 || graph.usesGroupReduction())
            source += "\n";

        const auto group = graph.threadGroupShape();

        source += "[numthreads(" + std::to_string(group.x) + ", "
                  + std::to_string(group.y) + ", " + std::to_string(group.z)
                  + ")]\n";
        source += "void computeMain(uint3 threadId : SV_DispatchThreadID";

        if (graph.usesLocalId())
            source += ", uint3 localThread : SV_GroupThreadID";

        if (graph.usesGroupId())
            source += ", uint3 groupIndex : SV_GroupID";

        // The flattened local index the scratch tree walks, which cs_5_0 hands
        // over as a semantic of its own rather than leaving it to be derived.
        // A kernel holding SIMD-group matrices needs it too: with no wave
        // matrix operation to lower to, it is what stands in for the SIMD
        // group's index and what picks the lane that makes a fragment's store.
        if (graph.usesGroupReduction() || graph.usesSimdGroups())
            source += ", uint groupLane : SV_GroupIndex";

        source += ")\n{\n";

        auto indexType = std::string(indexTypeName(rank));
        auto swizzle = std::string(indexSwizzle(rank));

        source += "    " + indexType + " gid = threadId" + swizzle + ";\n";

        if (graph.usesLocalId())
            source += "    " + indexType + " lid = localThread" + swizzle + ";\n";

        if (graph.usesGroupId())
            source += "    " + indexType + " tgid = groupIndex" + swizzle + ";\n";
    }

    // The early-return bounds guard the rounded-up dispatch needs - except in
    // a kernel that barriers, where a return some threads take ahead of a
    // barrier the rest sit at is undefined on both backends. There every
    // thread runs the whole body, and the kernel bounds its own stores
    // against gridCount()/gridWidth()/gridHeight()/gridDepth() instead.
    if (!graph.usesBarrier())
        source += boundsGuard(rank);

    // Stores ride the statement stream like everything else, so the body is
    // one block walk: a write records where it was made, inside whatever
    // loop or branch was open, and the emitter has no end-of-kernel step.
    auto stageRoots = Vector<int> {};
    collectStatementRoots(graph, ShaderGraph::rootBlock, stageRoots);

    auto stage = StageEmitter {graph, backend};

    source += stage.declareArrays(stageRoots, "    ");
    source += stage.emitBlock(ShaderGraph::rootBlock, "    ");

    source += "}\n";
    return source;
}

std::string emit(const ShaderGraph& graph, Backend backend)
{
    if (graph.isCompute())
        return emitCompute(graph, backend);

    auto source = std::string {};
    auto glsl = backend == Backend::Vulkan;

    auto promoted = promotedAttributes(graph);
    auto varyings = stageVaryings(graph, promoted);

    if (backend == Backend::Metal)
        source += "#include <metal_stdlib>\nusing namespace metal;\n\n";

    if (glsl)
        source += "#version 450\n\n";

    source += helperDefinitions(graph, backend);

    if (!glsl)
    {
        source += "struct VertexIn\n{\n";

        for (auto i = 0; i < graph.inputs().size(); ++i)
            source += "    " + std::string(typeName(backend, graph.inputs()[i]))
                      + " a" + std::to_string(i) + attributeSemantic(backend, i)
                      + ";\n";

        source += "};\n\nstruct VertexOut\n{\n";
        source += "    float4 position" + positionSemantic(backend) + ";\n";

        for (auto i = 0; i < varyings.size(); ++i)
            source += "    " + flatQualifier(backend, varyings[i].type)
                      + std::string(typeName(backend, varyings[i].type)) + " v"
                      + std::to_string(i)
                      + varyingSemantic(backend, i, varyings[i].type) + ";\n";

        source += "};\n\n";
    }

    auto hasUniforms = !graph.uniforms().empty();

    if (hasUniforms)
    {
        auto names = Vector<std::string> {};

        for (auto i = 0; i < graph.uniforms().size(); ++i)
            names.add("u" + std::to_string(i));

        source += uniformBlock(backend, graph.uniforms(), names);
    }

    if (glsl)
    {
        for (auto i = 0; i < graph.textureCount(); ++i)
            source +=
                glslSampledTexture(graph.textureKind(i), i, vulkanTextureBinding(i));

        if (graph.textureCount() > 0)
            source += "\n";

        // Read-only whatever the slot recorded: a render stage never writes.
        for (auto i = 0; i < graph.storageBuffers().size(); ++i)
            source += glslBufferBlock(BufferAccess::Read,
                                      graph.storageElementType(i),
                                      i,
                                      vulkanBufferBinding(i));

        if (graph.storageBuffers().size() > 0)
            source += "\n";
    }

    if (backend == Backend::DirectX)
    {
        // The texture lands on t<slot>. Its sampler does not land beside it:
        // the root signature declares one static sampler per sampling
        // configuration and every texture that declared that sampling reads the
        // same one, which is what keeps a slot count from costing sampler
        // registers. See TextureSampling, and hlslSamplerDeclarations.
        for (auto i = 0; i < graph.textureCount(); ++i)
            source += std::string(hlslTextureType(graph.textureKind(i))) + " texture"
                      + std::to_string(i) + " : register(t" + std::to_string(i)
                      + ");\n";

        source += hlslSamplerDeclarations(graph);

        if (graph.textureCount() > 0)
            source += "\n";

        // Storage buffers are globals here like the textures, at registers
        // above every texture slot - the render signature's mirror of the way
        // a kernel's textures sit above its buffers. See
        // RenderPass::bufferRegisterBase.
        for (auto i = 0; i < graph.storageBuffers().size(); ++i)
            source += std::string(hlslBufferType(BufferAccess::Read,
                                                 graph.storageElementType(i)))
                      + " buffer" + std::to_string(i) + " : register(t"
                      + std::to_string(RenderPass::bufferRegisterBase + i) + ");\n";

        if (graph.storageBuffers().size() > 0)
            source += "\n";
    }

    // On Metal each stage declares the uniform block as a function parameter,
    // and only when that stage's expressions read one; the HLSL cbuffer is a
    // global both functions already see. Slot 0 maps to buffer(uniformBase)
    // in both stages, matching what RenderPass::setVertexBytes /
    // setFragmentBytes bind. Uniforms live at buffer(uniformBase..) so a
    // vertex layout with multiple per-instance slots (0..N) never collides
    // with them.
    auto vertexRoots = vertexStageRoots(graph);

    if (backend == Backend::Metal)
    {
        source += "vertex VertexOut vertexMain(VertexIn input [[stage_in]]";

        if (hasUniforms && vertexReadsUniforms(graph))
            source += ", constant Uniforms& uniforms [[buffer("
                      + std::to_string(RenderPass::uniformBase) + ")]]";

        source += bufferParameters(graph, vertexRoots);
        source += ")\n{\n";
    }
    else if (glsl)
    {
        source += "#ifdef EACP_VERTEX\n";

        for (auto i = 0; i < graph.inputs().size(); ++i)
            source += locationLayout(i) + "in "
                      + std::string(typeName(backend, graph.inputs()[i])) + " attr"
                      + std::to_string(i) + ";\n";

        for (auto i = 0; i < varyings.size(); ++i)
            source += locationLayout(i) + flatQualifier(backend, varyings[i].type)
                      + "out " + std::string(typeName(backend, varyings[i].type))
                      + " vary" + std::to_string(i) + ";\n";

        source += "\nvoid main()\n{\n";
    }
    else
    {
        source += "VertexOut vertexMain(VertexIn input)\n{\n";
    }

    // The vertex stage takes no statements: a mutable local and the control flow
    // driving it belong to the fragment expression, the way sampling does. See
    // ShaderBuilder::var.
    auto vertexStage = StageEmitter {graph, backend};

    source += vertexStage.declareArrays(vertexRoots, "    ");
    source += vertexStage.defineFor(vertexRoots, "    ");

    auto varyingValue = [&](const StageVarying& varying)
    {
        if (varying.sourceNode >= 0)
            return vertexStage.printer.ref(varying.sourceNode);

        return attributeName(backend, varying.attribute);
    };

    // Clip y is left as the graph computed it: Vulkan's flipped NDC is a negative
    // viewport height in RenderPass, not a negation here, which flips winding.
    if (glsl)
    {
        source +=
            "    gl_Position = " + vertexStage.printer.ref(graph.position()) + ";\n";

        for (auto i = 0; i < varyings.size(); ++i)
            source += "    vary" + std::to_string(i) + " = "
                      + varyingValue(varyings[i]) + ";\n";

        source += "}\n#endif\n\n";
    }
    else
    {
        source += "    VertexOut output;\n";
        source += "    output.position = "
                  + vertexStage.printer.ref(graph.position()) + ";\n";

        for (auto i = 0; i < varyings.size(); ++i)
            source += "    output.v" + std::to_string(i) + " = "
                      + varyingValue(varyings[i]) + ";\n";

        source += "    return output;\n}\n\n";
    }

    // The alpha test is a fragment root like the colour is: its subtree is
    // planned with the colour's, so a value both of them read (the texture
    // sample, typically) is computed once and shared.
    auto fragmentRoots = Vector<int> {graph.fragment()};

    if (graph.discard() >= 0)
        fragmentRoots.add(graph.discard());

    // What the statements read counts towards the stage's uniform declaration,
    // but not towards the colour's locals: a statement's own expressions are
    // named where that statement is emitted, above.
    auto stageRoots = fragmentStageRoots(graph);

    if (backend == Backend::Metal)
    {
        source += "fragment float4 fragmentMain(VertexOut input [[stage_in]]";

        if (hasUniforms && fragmentReadsUniforms(graph))
            source += ",\n    constant Uniforms& uniforms [[buffer("
                      + std::to_string(RenderPass::uniformBase) + ")]]";

        for (auto i = 0; i < graph.textureCount(); ++i)
            source += ",\n    " + std::string(metalTextureType(graph.textureKind(i)))
                      + " texture" + std::to_string(i) + " [[texture("
                      + std::to_string(i) + ")]],\n    sampler sampler"
                      + std::to_string(i) + " [[sampler(" + std::to_string(i)
                      + ")]]";

        source += bufferParameters(graph, stageRoots);
        source += ")\n{\n";
    }
    else if (glsl)
    {
        source += "#ifdef EACP_FRAGMENT\n";

        for (auto i = 0; i < varyings.size(); ++i)
            source += locationLayout(i) + flatQualifier(backend, varyings[i].type)
                      + "in " + std::string(typeName(backend, varyings[i].type))
                      + " vary" + std::to_string(i) + ";\n";

        source += locationLayout(0) + "out vec4 fragColor;\n";
        source += "\nvoid main()\n{\n";
    }
    else
    {
        source += "float4 fragmentMain(VertexOut input) : SV_Target\n{\n";
    }

    // The shader's statements run first - they are what the fragment expression
    // then reads a mutable local out of - and the colour is planned after them.
    auto fragmentStage = StageEmitter {graph, backend, promoted.varyingOf};

    source += fragmentStage.declareArrays(stageRoots, "    ");
    source += fragmentStage.emitBlock(ShaderGraph::rootBlock, "    ");
    source += fragmentStage.defineFor(fragmentRoots, "    ");

    if (graph.discard() >= 0)
    {
        auto kill = backend == Backend::Metal ? "discard_fragment();" : "discard;";

        source += "    if (" + fragmentStage.printer.ref(graph.discard()) + " < "
                  + floatLiteral(graph.discardThreshold()) + ")\n        " + kill
                  + "\n";
    }

    if (glsl)
    {
        source += "    fragColor = " + fragmentStage.printer.ref(graph.fragment())
                  + ";\n}\n#endif\n";

        return source;
    }

    source += "    return " + fragmentStage.printer.ref(graph.fragment()) + ";\n}\n";

    return source;
}
} // namespace

bool vertexReadsUniforms(const ShaderGraph& graph)
{
    return anyReferencesUniform(graph, vertexStageRoots(graph));
}

bool fragmentReadsUniforms(const ShaderGraph& graph)
{
    return anyReferencesUniform(graph, fragmentStageRoots(graph));
}

std::string emitMetal(const ShaderGraph& graph)
{
    return emit(graph, Backend::Metal);
}

std::string emitHlsl(const ShaderGraph& graph)
{
    return emit(graph, Backend::DirectX);
}

std::string emitGlsl(const ShaderGraph& graph)
{
    return emit(graph, Backend::Vulkan);
}
} // namespace eacp::GPU
