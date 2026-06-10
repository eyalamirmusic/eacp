#include "ShaderEmitter.h"

#include "../Frame/ComputePass.h"
#include "ShaderGraph.h"
#include "UniformLayout.h"

#include <cstdio>
#include <string>
#include <vector>

// The single source-of-truth walker. MSL and HLSL differ only in the binding
// syntax and the stage scaffolding captured by the helpers below; the expression
// printer is fully shared because vector constructors, swizzles, operators and
// the float2/3/4 type names spell identically in both languages.

namespace eacp::GPU
{
namespace
{
enum class Backend
{
    Metal,
    DirectX
};

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

std::string attributeSemantic(Backend backend, int index)
{
    if (backend == Backend::Metal)
        return " [[attribute(" + std::to_string(index) + ")]]";

    return " : TEXCOORD" + std::to_string(index);
}

std::string varyingSemantic(Backend backend, int index)
{
    if (backend == Backend::Metal)
        return {};

    return " : TEXCOORD" + std::to_string(index);
}

std::string positionSemantic(Backend backend)
{
    if (backend == Backend::Metal)
        return " [[position]]";

    return " : SV_Position";
}

// Call nodes carry the canonical (MSL) builtin name; the few HLSL spells
// differently are translated here.
std::string callName(Backend backend, const std::string& name)
{
    if (backend == Backend::DirectX)
    {
        if (name == "fract")
            return "frac";

        if (name == "mix")
            return "lerp";
    }

    return name;
}

// Prints one stage's expressions. Nodes the stage plan named as locals print
// as tN references; everything else prints inline. print() spells out a node's
// own expression (used for both inline nodes and local definitions), ref() is
// what children and outputs go through, so shared subtrees collapse to a name.
struct ExprPrinter
{
    const ShaderGraph& graph;
    Backend backend;
    const std::vector<int>& locals; // node id -> local index, -1 = inline

    std::string ref(int node) const
    {
        if (locals[node] >= 0)
            return "t" + std::to_string(locals[node]);

        return print(node);
    }

    std::string print(int node) const
    {
        const auto& expr = graph.expr(node);

        switch (expr.kind)
        {
            case ExprKind::Input:
                return "input.a" + std::to_string(expr.index);

            case ExprKind::Varying:
                return "input.v" + std::to_string(expr.index);

            case ExprKind::Uniform:
                return "uniforms.u" + std::to_string(expr.index);

            case ExprKind::Constant:
                return floatLiteral(expr.value);

            case ExprKind::Construct:
            {
                auto text = std::string(typeName(expr.type)) + "(";

                for (auto i = 0; i < expr.args.size(); ++i)
                {
                    if (i > 0)
                        text += ", ";

                    text += ref(expr.args[i]);
                }

                text += ")";

                // float4x4(c0..c3) passes the four columns. MSL fills a matrix
                // from columns, but HLSL fills it from rows, so the same call
                // yields the transpose there; transpose() restores the
                // column-major value, so the mul() paths stay identical across
                // both backends.
                if (backend == Backend::DirectX && expr.type == ValueType::Float4x4)
                    return "transpose(" + text + ")";

                return text;
            }

            case ExprKind::Swizzle:
                return "(" + ref(expr.args[0]) + ")." + expr.text;

            case ExprKind::Call:
            {
                auto text = callName(backend, expr.text) + "(";

                for (auto i = 0; i < expr.args.size(); ++i)
                {
                    if (i > 0)
                        text += ", ";

                    text += ref(expr.args[i]);
                }

                return text + ")";
            }

            case ExprKind::Unary:
                // The operand gets its own parentheses: negating a negative
                // constant must print (-(-1.0)), never the pre-decrement
                // (--1.0).
                return "(" + std::string(1, expr.op) + "(" + ref(expr.args[0])
                       + "))";

            case ExprKind::Binary:
                return "(" + ref(expr.args[0]) + " " + std::string(1, expr.op) + " "
                       + ref(expr.args[1]) + ")";

            case ExprKind::Mul:
            {
                // Matrix * vector. MSL spells it with the * operator
                // (column-major); HLSL multiplies a matrix and vector with
                // mul().
                auto matrix = ref(expr.args[0]);
                auto vector = ref(expr.args[1]);

                if (backend == Backend::Metal)
                    return "(" + matrix + " * " + vector + ")";

                return "mul(" + matrix + ", " + vector + ")";
            }

            case ExprKind::Sample:
            {
                // Texture sample at a float2 coordinate, through the sampler
                // declared at the same index as the texture.
                auto name = "texture" + std::to_string(expr.index);
                auto sampler = "sampler" + std::to_string(expr.index);
                auto method = backend == Backend::Metal ? ".sample(" : ".Sample(";

                return name + method + sampler + ", " + ref(expr.args[0]) + ")";
            }

            case ExprKind::ThreadId:
                // Both kernel scaffoldings declare the 1D work-item id as gid.
                return "gid";

            case ExprKind::BufferRead:
                return "buffer" + std::to_string(expr.index) + "["
                       + ref(expr.args[0]) + "]";
        }

        return {};
    }
};

// Operation nodes are worth naming when evaluated more than once; leaf reads
// and swizzles stay inline - naming them saves nothing and hurts readability.
bool wantsLocal(ExprKind kind)
{
    switch (kind)
    {
        case ExprKind::Construct:
        case ExprKind::Call:
        case ExprKind::Unary:
        case ExprKind::Binary:
        case ExprKind::Mul:
        case ExprKind::Sample:
        case ExprKind::BufferRead:
            return true;

        case ExprKind::Input:
        case ExprKind::Varying:
        case ExprKind::Uniform:
        case ExprKind::Constant:
        case ExprKind::Swizzle:
        case ExprKind::ThreadId:
            return false;
    }

    return false;
}

// Counts how many references each node receives across the stage's roots: one
// per root plus one per parent edge, visiting each node's children only once.
void countUses(const ShaderGraph& graph,
               int node,
               std::vector<int>& uses,
               std::vector<char>& seen)
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

// Which nodes a stage emits as named locals, in dependency (post) order so
// every definition precedes its uses.
struct StagePlan
{
    std::vector<int> order;
    std::vector<int> locals; // node id -> local index, -1 = inline
};

void orderLocals(const ShaderGraph& graph,
                 int node,
                 const std::vector<int>& uses,
                 std::vector<char>& seen,
                 StagePlan& plan)
{
    if (node < 0 || seen[node])
        return;

    seen[node] = 1;

    for (auto argument: graph.expr(node).args)
        orderLocals(graph, argument, uses, seen, plan);

    if (uses[node] > 1 && wantsLocal(graph.expr(node).kind))
    {
        plan.locals[node] = (int) plan.order.size();
        plan.order.push_back(node);
    }
}

// Plans one stage: any operation the stage would evaluate more than once
// becomes a tN local, so shared subtrees are computed - and printed - once
// instead of being inlined at every use.
StagePlan planStage(const ShaderGraph& graph, const std::vector<int>& roots)
{
    auto count = (std::size_t) graph.nodeCount();

    auto plan = StagePlan {};
    plan.locals.assign(count, -1);

    auto uses = std::vector<int>(count, 0);
    auto seen = std::vector<char>(count, 0);

    for (auto root: roots)
        countUses(graph, root, uses, seen);

    auto ordered = std::vector<char>(count, 0);

    for (auto root: roots)
        orderLocals(graph, root, uses, ordered, plan);

    return plan;
}

std::string emitLocals(const ExprPrinter& printer, const StagePlan& plan)
{
    auto source = std::string {};

    for (auto node: plan.order)
        source += "    " + std::string(typeName(printer.graph.expr(node).type))
                  + " t" + std::to_string(plan.locals[node]) + " = "
                  + printer.print(node) + ";\n";

    return source;
}

// Whether the expression tree under node reads a uniform. A Varying read is the
// fragment-stage boundary: its vertex-stage source tree is walked separately as
// part of the vertex stage, so the walk stops there.
bool referencesUniform(const ShaderGraph& graph, int node)
{
    if (node < 0)
        return false;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::Uniform)
        return true;

    if (expr.kind == ExprKind::Varying)
        return false;

    for (auto argument: expr.args)
        if (referencesUniform(graph, argument))
            return true;

    return false;
}

bool vertexUsesUniforms(const ShaderGraph& graph)
{
    if (referencesUniform(graph, graph.position()))
        return true;

    for (const auto& varying: graph.varyings())
        if (referencesUniform(graph, varying.sourceNode))
            return true;

    return false;
}

// The Uniforms struct shared by both stages (and the HLSL cbuffer wrapping it).
// The CPU block is packed with MSL struct alignment (UniformLayout.h); HLSL
// cbuffer packing only forbids straddling a 16-byte register, so a vector after
// a scalar would land lower than the CPU wrote it - explicit pad scalars are
// emitted wherever the two rule sets disagree.
std::string uniformBlock(Backend backend,
                         const Vector<ValueType>& types,
                         const Vector<std::string>& names)
{
    auto source = std::string {"struct Uniforms\n{\n"};

    auto offsets = uniformOffsets(types);
    auto hlslCursor = 0;
    auto padCount = 0;

    for (auto i = 0; i < types.size(); ++i)
    {
        auto type = types[i];

        if (backend == Backend::DirectX)
        {
            while (hlslPackedOffset(hlslCursor, type) < offsets[i])
            {
                source += "    float pad" + std::to_string(padCount++) + ";\n";
                hlslCursor += 4;
            }

            hlslCursor = offsets[i] + byteSize(type);
        }

        source += "    " + std::string(typeName(type)) + " " + names[i] + ";\n";
    }

    source += "};\n\n";

    if (backend == Backend::DirectX)
        source += "cbuffer UniformsCB : register(b0)\n{\n"
                  "    Uniforms uniforms;\n};\n\n";

    return source;
}

// Compute kernel emission. The expression printer is the render one; only the
// scaffolding differs: storage buffers and the uniform block are MSL kernel
// parameters but HLSL globals, and the 1D work-item id arrives as a uint on
// Metal and as SV_DispatchThreadID.x on D3D. The block always ends with an
// implicit uint element count, and the kernel opens with the bounds guard the
// rounded-up dispatch needs; ComputeProgram appends the matching CPU value.
std::string emitCompute(const ShaderGraph& graph, Backend backend)
{
    auto source = std::string {};

    if (backend == Backend::Metal)
        source += "#include <metal_stdlib>\nusing namespace metal;\n\n";

    auto uniformTypes = graph.uniforms();
    auto uniformNames = Vector<std::string> {};

    for (auto i = 0; i < uniformTypes.size(); ++i)
        uniformNames.add("u" + std::to_string(i));

    uniformTypes.add(ValueType::UInt);
    uniformNames.add("count");

    source += uniformBlock(backend, uniformTypes, uniformNames);

    const auto& buffers = graph.storageBuffers();

    if (backend == Backend::Metal)
    {
        source += "kernel void computeMain(";

        for (auto i = 0; i < buffers.size(); ++i)
        {
            auto writable = buffers[i] == BufferAccess::Write;
            source += std::string(writable ? "device float* buffer"
                                           : "device const float* buffer")
                      + std::to_string(i) + " [[buffer(" + std::to_string(i)
                      + ")]],\n    ";
        }

        source += "constant Uniforms& uniforms [[buffer("
                  + std::to_string(ComputePass::uniformBase) + ")]],\n    ";
        source += "uint gid [[thread_position_in_grid]])\n{\n";
    }
    else
    {
        // SRV t<slot> / UAV u<slot> with one shared slot counter, matching the
        // flat Metal indices ComputePass binds both backends with.
        for (auto i = 0; i < buffers.size(); ++i)
        {
            auto slot = std::to_string(i);
            auto writable = buffers[i] == BufferAccess::Write;

            source += writable ? "RWStructuredBuffer<float> buffer"
                               : "StructuredBuffer<float> buffer";
            source += slot;
            source += writable ? " : register(u" : " : register(t";
            source += slot + ");\n";
        }

        if (buffers.size() > 0)
            source += "\n";

        source += "[numthreads(" + std::to_string(ComputePass::threadGroupWidth)
                  + ", 1, 1)]\n";
        source += "void computeMain(uint3 threadId : SV_DispatchThreadID)\n{\n";
        source += "    uint gid = threadId.x;\n";
    }

    source += "    if (gid >= uniforms.count)\n        return;\n";

    auto roots = std::vector<int> {};

    for (const auto& store: graph.stores())
    {
        roots.push_back(store.index);
        roots.push_back(store.value);
    }

    auto plan = planStage(graph, roots);
    auto printer = ExprPrinter {graph, backend, plan.locals};

    source += emitLocals(printer, plan);

    for (const auto& store: graph.stores())
        source += "    buffer" + std::to_string(store.slot) + "["
                  + printer.ref(store.index) + "] = " + printer.ref(store.value)
                  + ";\n";

    source += "}\n";
    return source;
}

std::string emit(const ShaderGraph& graph, Backend backend)
{
    if (graph.isCompute())
        return emitCompute(graph, backend);

    auto source = std::string {};

    if (backend == Backend::Metal)
        source += "#include <metal_stdlib>\nusing namespace metal;\n\n";

    source += "struct VertexIn\n{\n";

    for (auto i = 0; i < graph.inputs().size(); ++i)
        source += "    " + std::string(typeName(graph.inputs()[i])) + " a"
                  + std::to_string(i) + attributeSemantic(backend, i) + ";\n";

    source += "};\n\nstruct VertexOut\n{\n";
    source += "    float4 position" + positionSemantic(backend) + ";\n";

    for (auto i = 0; i < graph.varyings().size(); ++i)
        source += "    " + std::string(typeName(graph.varyings()[i].type)) + " v"
                  + std::to_string(i) + varyingSemantic(backend, i) + ";\n";

    source += "};\n\n";

    auto hasUniforms = !graph.uniforms().empty();

    // One uniform block aggregates every uniform<>() call. Both backends expose
    // it as "uniforms.uN" (HLSL wraps the struct in a cbuffer) so the expression
    // printer stays backend-agnostic.
    if (hasUniforms)
    {
        auto names = Vector<std::string> {};

        for (auto i = 0; i < graph.uniforms().size(); ++i)
            names.add("u" + std::to_string(i));

        source += uniformBlock(backend, graph.uniforms(), names);
    }

    // HLSL textures and samplers are globals; on Metal they are fragment
    // function parameters added to the signature below. Texture and sampler
    // share an index, matching RenderPass::setFragmentTexture.
    if (backend == Backend::DirectX)
    {
        for (auto i = 0; i < graph.textureCount(); ++i)
            source += "Texture2D texture" + std::to_string(i) + " : register(t"
                      + std::to_string(i) + ");\nSamplerState sampler"
                      + std::to_string(i) + " : register(s" + std::to_string(i)
                      + ");\n";

        if (graph.textureCount() > 0)
            source += "\n";
    }

    // On Metal each stage declares the uniform block as a function parameter,
    // and only when that stage's expressions read one; the HLSL cbuffer is a
    // global both functions already see. Slot 0 maps to buffer(1) in both
    // stages (vertex data owns buffer 0 on the vertex side; the fragment side
    // keeps the same index so one slot rule covers both).
    if (backend == Backend::Metal)
    {
        source += "vertex VertexOut vertexMain(VertexIn input [[stage_in]]";

        if (hasUniforms && vertexUsesUniforms(graph))
            source += ", constant Uniforms& uniforms [[buffer(1)]]";

        source += ")\n{\n";
    }
    else
    {
        source += "VertexOut vertexMain(VertexIn input)\n{\n";
    }

    auto vertexRoots = std::vector<int> {graph.position()};

    for (auto i = 0; i < graph.varyings().size(); ++i)
        vertexRoots.push_back(graph.varyings()[i].sourceNode);

    auto vertexPlan = planStage(graph, vertexRoots);
    auto vertexPrinter = ExprPrinter {graph, backend, vertexPlan.locals};

    source += emitLocals(vertexPrinter, vertexPlan);
    source += "    VertexOut output;\n";
    source += "    output.position = " + vertexPrinter.ref(graph.position()) + ";\n";

    for (auto i = 0; i < graph.varyings().size(); ++i)
        source += "    output.v" + std::to_string(i) + " = "
                  + vertexPrinter.ref(graph.varyings()[i].sourceNode) + ";\n";

    source += "    return output;\n}\n\n";

    if (backend == Backend::Metal)
    {
        source += "fragment float4 fragmentMain(VertexOut input [[stage_in]]";

        if (hasUniforms && referencesUniform(graph, graph.fragment()))
            source += ",\n    constant Uniforms& uniforms [[buffer(1)]]";

        for (auto i = 0; i < graph.textureCount(); ++i)
            source += ",\n    texture2d<float> texture" + std::to_string(i)
                      + " [[texture(" + std::to_string(i)
                      + ")]],\n    sampler sampler" + std::to_string(i)
                      + " [[sampler(" + std::to_string(i) + ")]]";

        source += ")\n{\n";
    }
    else
    {
        source += "float4 fragmentMain(VertexOut input) : SV_Target\n{\n";
    }

    auto fragmentPlan = planStage(graph, {graph.fragment()});
    auto fragmentPrinter = ExprPrinter {graph, backend, fragmentPlan.locals};

    source += emitLocals(fragmentPrinter, fragmentPlan);
    source += "    return " + fragmentPrinter.ref(graph.fragment()) + ";\n}\n";

    return source;
}
} // namespace

std::string emitMetal(const ShaderGraph& graph)
{
    return emit(graph, Backend::Metal);
}

std::string emitHlsl(const ShaderGraph& graph)
{
    return emit(graph, Backend::DirectX);
}
} // namespace eacp::GPU
