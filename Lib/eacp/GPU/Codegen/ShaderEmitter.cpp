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

std::string printExpr(const ShaderGraph& graph, int node, Backend backend)
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

                text += printExpr(graph, expr.args[i], backend);
            }

            return text + ")";
        }

        case ExprKind::Swizzle:
            return "(" + printExpr(graph, expr.args[0], backend) + ")." + expr.text;

        case ExprKind::Call:
        {
            auto text = expr.text + "(";

            for (auto i = 0; i < expr.args.size(); ++i)
            {
                if (i > 0)
                    text += ", ";

                text += printExpr(graph, expr.args[i], backend);
            }

            return text + ")";
        }

        case ExprKind::Binary:
            return "(" + printExpr(graph, expr.args[0], backend) + " "
                   + std::string(1, expr.op) + " "
                   + printExpr(graph, expr.args[1], backend) + ")";

        case ExprKind::Mul:
        {
            // Matrix * vector. MSL spells it with the * operator (column-major);
            // HLSL multiplies a matrix and vector with mul().
            auto matrix = printExpr(graph, expr.args[0], backend);
            auto vector = printExpr(graph, expr.args[1], backend);

            if (backend == Backend::Metal)
                return "(" + matrix + " * " + vector + ")";

            return "mul(" + matrix + ", " + vector + ")";
        }
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

    auto hasUniforms = !graph.uniforms().empty();

    // One uniform block aggregates every uniform<>() call. Both backends expose
    // it as "uniforms.uN" (HLSL wraps the struct in a cbuffer) so the expression
    // printer stays backend-agnostic.
    if (hasUniforms)
    {
        source += "struct Uniforms\n{\n";

        for (auto i = 0; i < graph.uniforms().size(); ++i)
            source += "    " + std::string(typeName(graph.uniforms()[i])) + " u"
                      + std::to_string(i) + ";\n";

        source += "};\n\n";

        if (backend == Backend::DirectX)
            source += "cbuffer UniformsCB : register(b0)\n{\n"
                      "    Uniforms uniforms;\n};\n\n";
    }

    if (backend == Backend::Metal)
    {
        source += "vertex VertexOut vertexMain(VertexIn input [[stage_in]]";

        // Vertex data owns buffer 0, so the uniform block lives at buffer 1.
        if (hasUniforms)
            source += ", constant Uniforms& uniforms [[buffer(1)]]";

        source += ")\n{\n";
    }
    else
    {
        source += "VertexOut vertexMain(VertexIn input)\n{\n";
    }

    source += "    VertexOut output;\n";
    source += "    output.position = " + printExpr(graph, graph.position(), backend)
              + ";\n";

    for (auto i = 0; i < graph.varyings().size(); ++i)
        source += "    output.v" + std::to_string(i) + " = "
                  + printExpr(graph, graph.varyings()[i].sourceNode, backend)
                  + ";\n";

    source += "    return output;\n}\n\n";

    if (backend == Backend::Metal)
        source += "fragment float4 fragmentMain(VertexOut input [[stage_in]])\n{\n";
    else
        source += "float4 fragmentMain(VertexOut input) : SV_Target\n{\n";

    source += "    return " + printExpr(graph, graph.fragment(), backend) + ";\n}\n";

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
