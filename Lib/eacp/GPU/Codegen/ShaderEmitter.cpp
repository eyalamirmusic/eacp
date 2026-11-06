#include "ShaderEmitter.h"

#include "../Frame/RenderPass.h"
#include "ShaderBindings.h"
#include "ShaderComputeEmitter.h"
#include "ShaderDeclarations.h"
#include "ShaderHelpers.h"
#include "ShaderStageEmitter.h"

#include <eacp/Core/Utils/Format.h>

#include <cassert>
#include <initializer_list>

namespace eacp::GPU
{
namespace
{
using namespace ShaderEmission;

constexpr auto stageIndent = std::string_view {"    "};

// Attributes the fragment stage reads, carried as varyings after the declared ones.
struct PromotedAttributes
{
    Vector<int> varyingOf; // attribute slot -> varying index, -1 = not read
    Vector<int> carried; // implicit varying order -> attribute slot
};

struct StageVarying
{
    ValueType type = ValueType::Float;
    int sourceNode = -1;
    int attribute = -1;
};

Vector<int> getVertexStageRoots(const ShaderGraph& graph)
{
    auto roots = Vector<int> {};
    roots.add(graph.position());

    for (const auto& varying: graph.varyings())
        roots.add(varying.sourceNode);

    return roots;
}

// The vertex outputs and every expression the vertex stage's statements read.
Vector<int> getVertexStatementRoots(const ShaderGraph& graph)
{
    auto roots = getVertexStageRoots(graph);

    if (graph.vertexBlock() >= 0)
        collectStatementExpressions(
            graph, graph.vertexBlock(), LoopConditions::Included, roots);

    return roots;
}

// The colour, the alpha test and the two optional outputs: what the fragment
// body plans its locals for after the statements have run.
Vector<int> getFragmentOutputRoots(const ShaderGraph& graph)
{
    auto roots = Vector<int> {graph.fragment()};

    for (auto optional: {graph.discard(), graph.fragmentDepth(), graph.sampleMask()})
        if (optional >= 0)
            roots.add(optional);

    return roots;
}

Vector<int> getFragmentStageRoots(const ShaderGraph& graph)
{
    auto roots = getFragmentOutputRoots(graph);

    collectStatementExpressions(
        graph, ShaderGraph::rootBlock, LoopConditions::Included, roots);
    return roots;
}

// A Varying read is the stage boundary. The visited set keeps the walk linear:
// shared subtrees would otherwise make it exponential.
bool doesReferenceUniform(const ShaderGraph& graph, int node, VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return false;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::Uniform)
        return true;

    if (expr.kind == ExprKind::Varying)
        return false;

    if (expr.kind == ExprKind::ArrayRead)
        for (auto element: graph.arrays()[expr.index].elements)
            if (doesReferenceUniform(graph, element, seen))
                return true;

    for (auto argument: expr.args)
        if (doesReferenceUniform(graph, argument, seen))
            return true;

    return false;
}

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

bool doesReachForeignBuiltIn(const ShaderGraph& graph,
                             int node,
                             ShaderStage stage,
                             VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return false;

    const auto& expr = graph.expr(node);

    if (expr.kind == ExprKind::Varying)
        return false;

    if (expr.kind == ExprKind::BuiltIn
        && stageOf(static_cast<ShaderBuiltIn>(expr.index)) != stage)
        return true;

    if (expr.kind == ExprKind::ArrayRead)
        for (auto element: graph.arrays()[expr.index].elements)
            if (doesReachForeignBuiltIn(graph, element, stage, seen))
                return true;

    for (auto argument: expr.args)
        if (doesReachForeignBuiltIn(graph, argument, stage, seen))
            return true;

    return false;
}

// Whether every built-in the stage's roots reach is one that stage is handed.
// A Varying read is the boundary: what feeds it is the vertex stage's.
bool readsOnlyItsOwnBuiltIns(const ShaderGraph& graph,
                             const Vector<int>& roots,
                             ShaderStage stage)
{
    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: roots)
        if (doesReachForeignBuiltIn(graph, root, stage, seen))
            return false;

    return true;
}

int findVaryingCarrying(const ShaderGraph& graph, int source)
{
    for (auto i = 0; i < graph.varyings().size(); ++i)
        if (graph.varyings()[i].sourceNode == source)
            return i;

    return -1;
}

Vector<char> collectFragmentAttributeReads(const ShaderGraph& graph)
{
    auto reached = Vector<char> {};
    reached.resize(graph.nodeCount(), 0);

    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: getFragmentStageRoots(graph))
        collectAttributeReads(graph, root, reached, seen);

    return reached;
}

void collectBufferSlots(const ShaderGraph& graph,
                        int node,
                        Vector<char>& used,
                        VisitSet& seen)
{
    if (node < 0 || !seen.visit(node))
        return;

    const auto& expr = graph.expr(node);

    auto readsSlot =
        expr.kind == ExprKind::BufferRead || expr.kind == ExprKind::BufferVectorRead;

    if (readsSlot && expr.index < used.size())
        used[expr.index] = 1;

    if (expr.kind == ExprKind::ArrayRead)
        for (auto element: graph.arrays()[expr.index].elements)
            collectBufferSlots(graph, element, used, seen);

    for (auto argument: expr.args)
        collectBufferSlots(graph, argument, used, seen);
}
bool doesAnyReferenceUniform(const ShaderGraph& graph, const Vector<int>& roots)
{
    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: roots)
        if (doesReferenceUniform(graph, root, seen))
            return true;

    return false;
}

// Every node one stage evaluates from its roots: arguments and the elements of
// the constant arrays it subscripts, stopping at a Varying, past which the
// value is the vertex stage's.
Vector<int> collectReachedNodes(const ShaderGraph& graph, const Vector<int>& roots)
{
    auto seen = VisitSet {graph.nodeCount()};
    auto pending = roots;
    auto reached = Vector<int> {};

    while (!pending.empty())
    {
        auto node = pending.back();
        pending.pop_back();

        if (node < 0 || !seen.visit(node))
            continue;

        reached.add(node);
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::Varying)
            continue;

        if (expr.kind == ExprKind::ArrayRead)
            for (auto element: graph.arrays()[expr.index].elements)
                pending.add(element);

        for (auto argument: expr.args)
            pending.add(argument);
    }

    return reached;
}

bool isDerivativeCall(const Expr& expr)
{
    return expr.text == "dfdx" || expr.text == "dfdy" || expr.text == "fwidth";
}

// A plain sample's level is derived unless it was given one; a layered or a
// comparison one has no level form at all.
bool takesDerivedLevel(const ShaderGraph& graph, const Expr& expr)
{
    switch (expr.kind)
    {
        case ExprKind::TextureFunction:
            return usesImplicitDerivatives(textureFunctionNamed(expr.text));
        case ExprKind::Sample:
            return expr.text == "compare" || isLayered(graph.textureKind(expr.index))
                   || expr.args.size() < 2;
        case ExprKind::Call:
            return isDerivativeCall(expr);
        default:
            return false;
    }
}

bool readsTextureSlot(ExprKind kind)
{
    return kind == ExprKind::Sample || kind == ExprKind::Fetch
           || kind == ExprKind::TextureFunction || kind == ExprKind::TextureQuery;
}

bool readsOnlyVariablesOf(const ShaderGraph& graph,
                          const Vector<int>& roots,
                          bool vertexStage)
{
    for (auto node: collectReachedNodes(graph, roots))
    {
        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::VarRead
            && graph.isVertexVariable(expr.index) != vertexStage)
            return false;
    }

    return true;
}

PromotedAttributes findPromotedAttributes(const ShaderGraph& graph)
{
    auto promoted = PromotedAttributes {};
    promoted.varyingOf.resize(graph.inputs().size(), -1);

    auto reached = collectFragmentAttributeReads(graph);

    // Ascending node order, so the implicit varyings follow declaration order.
    for (auto node = 0; node < graph.nodeCount(); ++node)
    {
        if (reached[node] == 0)
            continue;

        auto slot = graph.expr(node).index;
        auto carrier = findVaryingCarrying(graph, node);

        if (carrier < 0)
        {
            carrier = graph.varyings().size() + promoted.carried.size();
            promoted.carried.add(slot);
        }

        promoted.varyingOf[slot] = carrier;
    }

    return promoted;
}

Vector<StageVarying> getStageVaryings(const ShaderGraph& graph,
                                      const PromotedAttributes& promoted)
{
    auto slots = Vector<StageVarying> {};

    for (const auto& varying: graph.varyings())
        slots.add(StageVarying {varying.type, varying.sourceNode, -1});

    for (auto attribute: promoted.carried)
        slots.add(StageVarying {graph.inputs()[attribute], -1, attribute});

    return slots;
}

Vector<char> findBufferSlotsUsedBy(const ShaderGraph& graph,
                                   const Vector<int>& roots)
{
    auto used = Vector<char> {};
    used.resize(graph.storageBuffers().size(), 0);

    auto seen = VisitSet {graph.nodeCount()};

    for (auto root: roots)
        collectBufferSlots(graph, root, used, seen);

    return used;
}
std::string formatBufferParameters(const ShaderGraph& graph,
                                   const Vector<int>& roots)
{
    auto used = findBufferSlotsUsedBy(graph, roots);
    auto source = std::string {};

    for (auto i = 0; i < used.size(); ++i)
        if (used[i] != 0)
            source += Strings::format(
                ",\n    {} buffer{} [[buffer({})]]",
                getMetalBufferType(BufferAccess::Read, graph.storageElementType(i)),
                i,
                RenderPass::bufferBase + i);

    return source;
}

class RenderShaderEmitter
{
public:
    RenderShaderEmitter(const ShaderGraph& graphToUse, Backend backendToUse)
        : graph(graphToUse)
        , backend(backendToUse)
        , promoted(findPromotedAttributes(graphToUse))
        , varyings(getStageVaryings(graphToUse, promoted))
        , vertexRoots(getVertexStageRoots(graphToUse))
        , vertexStatementRoots(getVertexStatementRoots(graphToUse))
        , fragmentStatementRoots(getFragmentStageRoots(graphToUse))
    {
    }

    std::string emit() const
    {
        assert(
            readsOnlyItsOwnBuiltIns(graph, vertexStatementRoots, ShaderStage::Vertex)
            && "eacp: the vertex stage read a built-in it is not handed - a "
               "fragment one such as fragmentCoordinate or frontFacing, or a "
               "kernel's.");

        assert(!vertexUsesImplicitDerivatives(graph)
               && "eacp: the vertex stage used sample, sampleBias, "
                  "sampleCompare, a layered sample or dfdx/dfdy/fwidth. They "
                  "take the level from the fragment's derivatives, which a "
                  "vertex has none of - use sampleLevel, sampleGrad, gather, "
                  "load or sampleCompareLevel.");

        assert(stagesReadOnlyTheirOwnVariables(graph)
               && "eacp: a stage read a variable the other stage declared. A "
                  "vertexStage variable reaches the fragment through "
                  "varying(); a fragment one never feeds the position or a "
                  "varying.");

        assert(readsOnlyItsOwnBuiltIns(
                   graph, fragmentStatementRoots, ShaderStage::Fragment)
               && "eacp: the fragment stage read a built-in it is not handed. "
                  "vertexId and instanceId reach it through varying().");

        assert(declaresNoWritableTexture()
               && "eacp: a render program declared a Write or ReadWrite "
                  "texture. Only a kernel writes one; a fragment shader reads a "
                  "storage texture declared StorageAccess::Read.");

        auto source = getSourcePreamble(graph, backend);

        if (!isGlsl())
            source += printStageStructs();

        source += printUniformBlock();

        if (isGlsl())
            source += printGlslBindings();

        if (backend == Backend::DirectX)
            source += printHlslBindings();

        source += printVertexOpening();
        source += printVertexBody();
        source += printFragmentOpening();
        source += printFragmentBody();
        return source;
    }

private:
    bool isGlsl() const { return backend == Backend::Vulkan; }

    bool hasUniforms() const { return !graph.uniforms().empty(); }

    bool declaresNoWritableTexture() const
    {
        for (auto i = 0; i < graph.textureCount(); ++i)
            if (graph.textureAccess(i) == TextureAccess::Write
                || graph.textureAccess(i) == TextureAccess::ReadWrite)
                return false;

        return true;
    }

    std::string printStageStructs() const
    {
        auto source = std::string {"struct VertexIn\n{\n"};

        for (auto i = 0; i < graph.inputs().size(); ++i)
            source += Strings::format("    {} a{}{};\n",
                                      getDialectTypeName(backend, graph.inputs()[i]),
                                      i,
                                      getAttributeSemantic(backend, i));

        source += "};\n\nstruct VertexOut\n{\n";
        source += Strings::format("    float4 position{};\n",
                                  getPositionSemantic(backend));

        for (auto i = 0; i < varyings.size(); ++i)
        {
            auto type = varyings[i].type;

            source += Strings::format("    {}{} v{}{};\n",
                                      getFlatQualifier(backend, type),
                                      getDialectTypeName(backend, type),
                                      i,
                                      getVaryingSemantic(backend, i, type));
        }

        source += "};\n\n";
        return source + printFragmentOutStruct();
    }

    bool hasFragmentOutStruct() const
    {
        return graph.fragmentDepth() >= 0 || graph.sampleMask() >= 0;
    }

    std::string printFragmentOutStruct() const
    {
        if (!hasFragmentOutStruct())
            return {};

        auto isMetal = backend == Backend::Metal;
        auto source = std::string {"struct FragmentOut\n{\n"};
        source += isMetal ? "    float4 color [[color(0)]];\n"
                          : "    float4 color : SV_Target;\n";

        if (graph.fragmentDepth() >= 0)
            source += isMetal ? "    float depth [[depth(any)]];\n"
                              : "    float depth : SV_Depth;\n";

        if (graph.sampleMask() >= 0)
            source += isMetal ? "    uint sampleMask [[sample_mask]];\n"
                              : "    uint sampleMask : SV_Coverage;\n";

        return source + "};\n\n";
    }

    // The fragment inputs past the stage_in struct, each a parameter of its
    // own on Metal and HLSL. GLSL reads its gl_ variables instead.
    std::string printFragmentBuiltInParameters(std::string_view separator) const
    {
        struct Parameter
        {
            ShaderBuiltIn which;
            std::string_view declaration;
            std::string_view metal;
            std::string_view hlsl;
        };

        static constexpr auto parameters = Array<Parameter, 3> {{
            {ShaderBuiltIn::FrontFacing,
             "bool frontFacing",
             "[[front_facing]]",
             ": SV_IsFrontFace"},
            {ShaderBuiltIn::SampleIndex,
             "uint sampleIndex",
             "[[sample_id]]",
             ": SV_SampleIndex"},
            {ShaderBuiltIn::SampleMask,
             "uint sampleMaskIn",
             "[[sample_mask]]",
             ": SV_Coverage"},
        }};

        auto source = std::string {};

        for (const auto& parameter: parameters)
            if (graph.usesBuiltIn(parameter.which))
                source += Strings::format(
                    "{}{} {}",
                    separator,
                    parameter.declaration,
                    backend == Backend::Metal ? parameter.metal : parameter.hlsl);

        return source;
    }

    std::string printUniformBlock() const
    {
        if (!hasUniforms())
            return {};

        auto names = Vector<std::string> {};

        for (auto i = 0; i < graph.uniforms().size(); ++i)
            names.add(Strings::format("u{}", i));

        return formatUniformBlock(backend, graph.uniforms(), names);
    }

    std::string printGlslBindings() const
    {
        auto source = std::string {};

        for (auto i = 0; i < graph.textureCount(); ++i)
            source += formatGlslTexture(graph, i, vulkanTextureBinding(i));

        if (graph.textureCount() > 0)
            source += "\n";

        for (auto i = 0; i < graph.storageBuffers().size(); ++i)
            source += formatGlslBufferBlock(BufferAccess::Read,
                                            graph.storageElementType(i),
                                            i,
                                            vulkanBufferBinding(i));

        if (graph.storageBuffers().size() > 0)
            source += "\n";

        return source;
    }

    // Storage buffers sit at registers above every texture slot.
    std::string printHlslBindings() const
    {
        auto source = std::string {};

        for (auto i = 0; i < graph.textureCount(); ++i)
        {
            if (graph.textureAccess(i) == TextureAccess::Sampler)
                source += formatHlslSampler(graph, i);
            else
                source += Strings::format("{} texture{} : register(t{});\n",
                                          getHlslTextureDeclarationType(graph, i),
                                          i,
                                          i);
        }

        source += formatHlslSamplerDeclarations(graph);

        if (graph.textureCount() > 0)
            source += "\n";

        for (auto i = 0; i < graph.storageBuffers().size(); ++i)
            source += Strings::format(
                "{} buffer{} : register(t{});\n",
                getHlslBufferType(BufferAccess::Read, graph.storageElementType(i)),
                i,
                RenderPass::bufferRegisterBase + i);

        if (graph.storageBuffers().size() > 0)
            source += "\n";

        return source;
    }

    std::string printGlslVaryings(std::string_view direction) const
    {
        auto source = std::string {};

        for (auto i = 0; i < varyings.size(); ++i)
        {
            auto type = varyings[i].type;

            source += Strings::format("{}{}{} {} vary{};\n",
                                      getLocationLayout(i),
                                      getFlatQualifier(backend, type),
                                      direction,
                                      getDialectTypeName(backend, type),
                                      i);
        }

        return source;
    }

    std::string printMetalUniformParameter(std::string_view separator) const
    {
        return Strings::format("{}constant Uniforms& uniforms [[buffer({})]]",
                               separator,
                               RenderPass::uniformBase);
    }

    std::string printMetalVertexOpening() const
    {
        auto source =
            std::string {"vertex VertexOut vertexMain(VertexIn input [[stage_in]]"};

        if (graph.usesVertexId())
            source += ", uint vertexId [[vertex_id]]";

        if (graph.usesInstanceId())
            source += ", uint instanceId [[instance_id]]";

        if (hasUniforms() && vertexReadsUniforms(graph))
            source += printMetalUniformParameter(", ");

        const auto vertexTextures = vertexTextureSlots(graph);

        for (auto i = 0; i < graph.textureCount(); ++i)
            if (((vertexTextures >> i) & 1u) != 0u)
                source += printMetalTextureParameter(i);

        source += formatBufferParameters(graph, vertexStatementRoots);
        return source + ")\n{\n";
    }

    std::string printMetalTextureParameter(int slot) const
    {
        constexpr auto samplerParameter =
            std::string_view {",\n    sampler sampler{} [[sampler({})]]"};

        if (graph.textureAccess(slot) == TextureAccess::Sampler)
            return Strings::format(samplerParameter, slot, slot);

        auto source = Strings::format(",\n    {} texture{} [[texture({})]]",
                                      getMetalTextureParameterType(graph, slot),
                                      slot,
                                      slot);

        if (graph.textureAccess(slot) == TextureAccess::Sample)
            source += Strings::format(samplerParameter, slot, slot);

        return source;
    }

    std::string printGlslVertexOpening() const
    {
        auto source = std::string {"#ifdef EACP_VERTEX\n"};

        for (auto i = 0; i < graph.inputs().size(); ++i)
            source += Strings::format("{}in {} attr{};\n",
                                      getLocationLayout(i),
                                      getDialectTypeName(backend, graph.inputs()[i]),
                                      i);

        source += printGlslVaryings("out");
        return source + "\nvoid main()\n{\n";
    }

    std::string printHlslVertexOpening() const
    {
        auto source = std::string {"VertexOut vertexMain(VertexIn input"};

        if (graph.usesVertexId())
            source += ", uint vertexId : SV_VertexID";

        if (graph.usesInstanceId())
            source += ", uint instanceId : SV_InstanceID";

        return source + ")\n{\n";
    }

    std::string printVertexOpening() const
    {
        if (backend == Backend::Metal)
            return printMetalVertexOpening();

        if (isGlsl())
            return printGlslVertexOpening();

        return printHlslVertexOpening();
    }

    // Clip y is left as computed: Vulkan's flip is a negative viewport height.
    // The statements run first, as the fragment's do, so the position and the
    // varyings read the variables they leave behind.
    std::string printVertexBody() const
    {
        auto stage = StageEmitter {graph, backend};
        stage.returnRoots = vertexRoots;

        auto printEarlyReturn =
            [this](const StageEmitter& emitter, const std::string& indent)
        { return printVertexReturn(emitter, indent); };
        stage.printReturn = printEarlyReturn;

        auto source = stage.declareArrays(vertexStatementRoots, stageIndent);

        if (graph.vertexBlock() >= 0)
            source += stage.emitBlock(graph.vertexBlock(), stageIndent);

        source += stage.defineLocalsFor(vertexRoots, stageIndent);

        if (isGlsl())
            return source + printVertexOutputs(stage, stageIndent) + "}\n#endif\n\n";

        return source + printVertexOutputs(stage, stageIndent)
               + "    return output;\n}\n\n";
    }

    // The position and the varyings, and on Metal and HLSL the struct holding
    // them; the function's last lines, and an early return's.
    std::string printVertexOutputs(const StageEmitter& stage,
                                   std::string_view indent) const
    {
        auto position = stage.printer.printReference(graph.position());

        if (isGlsl())
            return Strings::format("{}gl_Position = {};\n", indent, position)
                   + printVaryingAssignments(stage, "vary", indent);

        return Strings::format("{}VertexOut output;\n{}output.position = {};\n",
                               indent,
                               indent,
                               position)
               + printVaryingAssignments(stage, "output.v", indent);
    }

    // Braced on Metal and HLSL, so the struct it declares is its own.
    std::string printVertexReturn(const StageEmitter& stage,
                                  const std::string& indent) const
    {
        if (isGlsl())
            return printVertexOutputs(stage, indent) + indent + "return;\n";

        auto inner = indent + std::string(stageIndent);
        return indent + "{\n" + printVertexOutputs(stage, inner) + inner
               + "return output;\n" + indent + "}\n";
    }

    std::string printVaryingAssignments(const StageEmitter& stage,
                                        std::string_view prefix,
                                        std::string_view indent) const
    {
        auto source = std::string {};

        for (auto i = 0; i < varyings.size(); ++i)
            source += Strings::format("{}{}{} = {};\n",
                                      indent,
                                      prefix,
                                      i,
                                      printVaryingValue(stage, varyings[i]));

        return source;
    }

    std::string printVaryingValue(const StageEmitter& stage,
                                  const StageVarying& varying) const
    {
        if (varying.sourceNode >= 0)
            return stage.printer.printReference(varying.sourceNode);

        return getAttributeName(backend, varying.attribute);
    }

    std::string printMetalFragmentOpening() const
    {
        auto source =
            Strings::format("fragment {} fragmentMain(VertexOut input [[stage_in]]",
                            getFragmentReturnType());

        source += printFragmentBuiltInParameters(",\n    ");

        if (hasUniforms() && fragmentReadsUniforms(graph))
            source += printMetalUniformParameter(",\n    ");

        for (auto i = 0; i < graph.textureCount(); ++i)
            source += printMetalTextureParameter(i);

        source += formatBufferParameters(graph, fragmentStatementRoots);
        return source + ")\n{\n";
    }

    std::string_view getFragmentReturnType() const
    {
        return hasFragmentOutStruct() ? "FragmentOut" : "float4";
    }

    std::string printHlslFragmentOpening() const
    {
        auto source = Strings::format("{} fragmentMain(VertexOut input",
                                      getFragmentReturnType());
        source += printFragmentBuiltInParameters(", ");
        source += ")";

        if (!hasFragmentOutStruct())
            source += " : SV_Target";

        return source + "\n{\n";
    }

    std::string printGlslFragmentOpening() const
    {
        auto source = std::string {"#ifdef EACP_FRAGMENT\n"};
        source += printGlslVaryings("in");
        source += getLocationLayout(0) + "out vec4 fragColor;\n";
        return source + "\nvoid main()\n{\n";
    }

    std::string printFragmentOpening() const
    {
        if (backend == Backend::Metal)
            return printMetalFragmentOpening();

        if (isGlsl())
            return printGlslFragmentOpening();

        return printHlslFragmentOpening();
    }

    // The alpha test and the optional outputs are planned with the colour, so
    // a value more than one of them reads is shared.
    std::string printFragmentBody() const
    {
        auto stage = StageEmitter {graph, backend, promoted.varyingOf};
        stage.returnRoots = getFragmentOutputRoots(graph);

        auto printEarlyReturn =
            [this](const StageEmitter& emitter, const std::string& indent)
        { return printFragmentReturn(emitter, indent); };
        stage.printReturn = printEarlyReturn;

        auto source = stage.declareArrays(fragmentStatementRoots, stageIndent);
        source += stage.emitBlock(ShaderGraph::rootBlock, stageIndent);
        source += stage.defineLocalsFor(getFragmentOutputRoots(graph), stageIndent);
        source += printAlphaTest(stage, stageIndent);

        auto colour = stage.printer.printReference(graph.fragment());

        if (isGlsl())
            return source + printGlslFragmentOutputs(stage, colour, stageIndent)
                   + "}\n#endif\n";

        if (!hasFragmentOutStruct())
            return source + Strings::format("    return {};\n}}\n", colour);

        return source + printFragmentOutAssignments(stage, colour, stageIndent)
               + "    return output;\n}\n";
    }

    // The alpha test and the outputs where an early return stands; braced
    // where it declares the FragmentOut struct, so the struct is its own.
    std::string printFragmentReturn(const StageEmitter& stage,
                                    const std::string& indent) const
    {
        auto source = printAlphaTest(stage, indent);
        auto colour = stage.printer.printReference(graph.fragment());

        if (isGlsl())
            return source + printGlslFragmentOutputs(stage, colour, indent) + indent
                   + "return;\n";

        if (!hasFragmentOutStruct())
            return source + Strings::format("{}return {};\n", indent, colour);

        auto inner = indent + std::string(stageIndent);
        return source + indent + "{\n"
               + printFragmentOutAssignments(stage, colour, inner) + inner
               + "return output;\n" + indent + "}\n";
    }

    std::string printFragmentOutAssignments(const StageEmitter& stage,
                                            const std::string& colour,
                                            std::string_view indent) const
    {
        auto source = Strings::format(
            "{}FragmentOut output;\n{}output.color = {};\n", indent, indent, colour);

        if (graph.fragmentDepth() >= 0)
            source +=
                Strings::format("{}output.depth = {};\n",
                                indent,
                                stage.printer.printReference(graph.fragmentDepth()));

        if (graph.sampleMask() >= 0)
            source +=
                Strings::format("{}output.sampleMask = {};\n",
                                indent,
                                stage.printer.printReference(graph.sampleMask()));

        return source;
    }

    std::string printGlslFragmentOutputs(const StageEmitter& stage,
                                         const std::string& colour,
                                         std::string_view indent) const
    {
        auto source = Strings::format("{}fragColor = {};\n", indent, colour);

        if (graph.fragmentDepth() >= 0)
            source +=
                Strings::format("{}gl_FragDepth = {};\n",
                                indent,
                                stage.printer.printReference(graph.fragmentDepth()));

        if (graph.sampleMask() >= 0)
            source +=
                Strings::format("{}gl_SampleMask[0] = int({});\n",
                                indent,
                                stage.printer.printReference(graph.sampleMask()));

        return source;
    }

    std::string printAlphaTest(const StageEmitter& stage,
                               std::string_view indent) const
    {
        if (graph.discard() < 0)
            return {};

        auto kill = backend == Backend::Metal ? "discard_fragment();" : "discard;";

        return Strings::format("{}if ({} < {})\n{}{}{}\n",
                               indent,
                               stage.printer.printReference(graph.discard()),
                               formatShaderFloatLiteral(graph.discardThreshold()),
                               indent,
                               stageIndent,
                               kill);
    }

    const ShaderGraph& graph;
    Backend backend;
    PromotedAttributes promoted;
    Vector<StageVarying> varyings;
    Vector<int> vertexRoots;
    Vector<int> vertexStatementRoots;
    Vector<int> fragmentStatementRoots;
};

std::string emit(const ShaderGraph& graph, Backend backend)
{
    if (graph.isCompute())
        return emitCompute(graph, backend);

    return RenderShaderEmitter {graph, backend}.emit();
}
} // namespace

bool vertexReadsUniforms(const ShaderGraph& graph)
{
    return doesAnyReferenceUniform(graph, getVertexStatementRoots(graph));
}

bool fragmentReadsUniforms(const ShaderGraph& graph)
{
    return doesAnyReferenceUniform(graph, getFragmentStageRoots(graph));
}

std::uint32_t vertexTextureSlots(const ShaderGraph& graph)
{
    if (graph.isCompute())
        return 0u;

    auto slots = std::uint32_t {0u};
    const auto bitOf = [](int slot) { return std::uint32_t {1u} << slot; };

    for (auto node: collectReachedNodes(graph, getVertexStatementRoots(graph)))
    {
        const auto& expr = graph.expr(node);

        if (readsTextureSlot(expr.kind))
            slots |= bitOf(expr.index);

        if (expr.kind == ExprKind::TextureFunction && expr.sampler >= 0)
            slots |= bitOf(expr.sampler);
    }

    return slots;
}

bool vertexUsesImplicitDerivatives(const ShaderGraph& graph)
{
    for (auto node: collectReachedNodes(graph, getVertexStatementRoots(graph)))
        if (takesDerivedLevel(graph, graph.expr(node)))
            return true;

    return false;
}

bool stagesReadOnlyTheirOwnVariables(const ShaderGraph& graph)
{
    return readsOnlyVariablesOf(graph, getVertexStatementRoots(graph), true)
           && readsOnlyVariablesOf(graph, getFragmentStageRoots(graph), false);
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
