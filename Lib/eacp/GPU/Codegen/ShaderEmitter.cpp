#include "ShaderEmitter.h"

#include "ShaderGraph.h"

#include <cstdio>
#include <string>

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

std::string printExpr(const ShaderGraph& graph, int node)
{
    const auto& expr = graph.expr(node);

    switch (expr.kind)
    {
        case ExprKind::Input:
            return "input.a" + std::to_string(expr.index);

        case ExprKind::Varying:
            return "input.v" + std::to_string(expr.index);

        case ExprKind::Constant:
            return floatLiteral(expr.value);

        case ExprKind::Construct:
        {
            auto text = std::string(typeName(expr.type)) + "(";

            for (auto i = 0; i < expr.args.size(); ++i)
            {
                if (i > 0)
                    text += ", ";

                text += printExpr(graph, expr.args[i]);
            }

            return text + ")";
        }

        case ExprKind::Swizzle:
            return "(" + printExpr(graph, expr.args[0]) + ")." + expr.swizzle;

        case ExprKind::Binary:
            return "(" + printExpr(graph, expr.args[0]) + " "
                   + std::string(1, expr.op) + " " + printExpr(graph, expr.args[1])
                   + ")";
    }

    return {};
}

std::string emit(const ShaderGraph& graph, Backend backend)
{
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

    if (backend == Backend::Metal)
        source += "vertex VertexOut vertexMain(VertexIn input [[stage_in]])\n{\n";
    else
        source += "VertexOut vertexMain(VertexIn input)\n{\n";

    source += "    VertexOut output;\n";
    source += "    output.position = " + printExpr(graph, graph.position()) + ";\n";

    for (auto i = 0; i < graph.varyings().size(); ++i)
        source += "    output.v" + std::to_string(i) + " = "
                  + printExpr(graph, graph.varyings()[i].sourceNode) + ";\n";

    source += "    return output;\n}\n\n";

    if (backend == Backend::Metal)
        source += "fragment float4 fragmentMain(VertexOut input [[stage_in]])\n{\n";
    else
        source += "float4 fragmentMain(VertexOut input) : SV_Target\n{\n";

    source += "    return " + printExpr(graph, graph.fragment()) + ";\n}\n";

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
