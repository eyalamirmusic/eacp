#include "ShaderOverrideEmission.h"

#include "OverrideValues.h"

#include <eacp/Core/Utils/Format.h>

namespace eacp::GPU::ShaderEmission
{
namespace
{
PipelineConstant declaredDefaultOf(const OverrideDeclaration& declaration, int id)
{
    auto constant = PipelineConstant {};
    constant.id = id;
    constant.type = pipelineConstantTypeOf(declaration.type);
    constant.bits = declaration.hasDefault ? declaration.defaultBits : 0u;
    return constant;
}

std::string formatMetalOverride(const ShaderGraph& graph, int id)
{
    const auto& declaration = graph.overrides()[id];
    const auto type = typeName(declaration.type);
    const auto name = getOverrideName(graph, id);

    if (!declaration.hasDefault)
        return Strings::format(
            "constant {} {} [[function_constant({})]];\n", type, name, id);

    const auto raw = Strings::format("eacpOverride{}", id);

    return Strings::format(
        "constant {} {} [[function_constant({})]];\n"
        "constant {} {} = is_function_constant_defined({}) ? {} : {};\n",
        type,
        raw,
        id,
        type,
        name,
        raw,
        raw,
        printOverrideLiteral(Backend::Metal, declaredDefaultOf(declaration, id)));
}

std::string formatGlslOverride(const ShaderGraph& graph, int id)
{
    const auto& declaration = graph.overrides()[id];

    return Strings::format(
        "layout(constant_id = {}) const {} {} = {};\n",
        id,
        glslTypeName(declaration.type),
        getOverrideName(graph, id),
        printOverrideLiteral(Backend::Vulkan, declaredDefaultOf(declaration, id)));
}

std::string formatHlslOverride(const ShaderGraph& graph, int id)
{
    const auto& declaration = graph.overrides()[id];
    const auto macro = hlslOverrideMacro(id);

    return Strings::format(
        "#ifndef {}\n#define {} {}\n#endif\nstatic const {} {} = {};\n",
        macro,
        macro,
        printOverrideLiteral(Backend::DirectX, declaredDefaultOf(declaration, id)),
        hlslTypeName(declaration.type),
        getOverrideName(graph, id),
        macro);
}

std::string formatOverride(const ShaderGraph& graph, Backend backend, int id)
{
    switch (backend)
    {
        case Backend::Metal:
            return formatMetalOverride(graph, id);
        case Backend::DirectX:
            return formatHlslOverride(graph, id);
        case Backend::Vulkan:
            break;
    }

    return formatGlslOverride(graph, id);
}
} // namespace

std::string getOverrideDeclarations(const ShaderGraph& graph, Backend backend)
{
    if (graph.overrides().empty())
        return {};

    auto source = std::string {};

    for (auto id = 0; id < graph.overrides().size(); ++id)
        source += formatOverride(graph, backend, id);

    return source + "\n";
}

std::string getOverrideName(const ShaderGraph& graph, int id)
{
    return Strings::format("override_{}", graph.overrides()[id].name);
}

std::string printOverrideLiteral(Backend backend, const PipelineConstant& value)
{
    const auto literal = hlslLiteralOf(value);

    if (value.type != PipelineConstantType::Half)
        return literal;

    return literal + std::string(getHalfLiteralSuffix(backend));
}
} // namespace eacp::GPU::ShaderEmission
