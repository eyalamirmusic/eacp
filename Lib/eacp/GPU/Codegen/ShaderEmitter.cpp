#include "ShaderEmitter.h"

#include "../Frame/RenderPass.h"
#include "ShaderBindings.h"
#include "ShaderComputeEmitter.h"
#include "ShaderDeclarations.h"
#include "ShaderHelpers.h"
#include "ShaderStageEmitter.h"

#include <eacp/Core/Utils/Format.h>

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

Vector<int> getFragmentStageRoots(const ShaderGraph& graph)
{
    auto roots = Vector<int> {graph.fragment()};

    if (graph.discard() >= 0)
        roots.add(graph.discard());

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
        , fragmentStatementRoots(getFragmentStageRoots(graphToUse))
    {
    }

    std::string emit() const
    {
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

        return source + "};\n\n";
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
            source += formatGlslSampledTexture(graph, i, vulkanTextureBinding(i));

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
            source += Strings::format("{} texture{} : register(t{});\n",
                                      getHlslTextureType(graph.textureKind(i)),
                                      i,
                                      i);

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

        source += formatBufferParameters(graph, vertexRoots);
        return source + ")\n{\n";
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
    std::string printVertexBody() const
    {
        auto stage = StageEmitter {graph, backend};

        auto source = stage.declareArrays(vertexRoots, stageIndent);
        source += stage.defineLocalsFor(vertexRoots, stageIndent);

        auto position = stage.printer.printReference(graph.position());

        if (isGlsl())
        {
            source += Strings::format("    gl_Position = {};\n", position);
            source += printVaryingAssignments(stage, "vary");
            return source + "}\n#endif\n\n";
        }

        source += "    VertexOut output;\n";
        source += Strings::format("    output.position = {};\n", position);
        source += printVaryingAssignments(stage, "output.v");
        return source + "    return output;\n}\n\n";
    }

    std::string printVaryingAssignments(const StageEmitter& stage,
                                        std::string_view prefix) const
    {
        auto source = std::string {};

        for (auto i = 0; i < varyings.size(); ++i)
            source += Strings::format("    {}{} = {};\n",
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
        auto source = std::string {
            "fragment float4 fragmentMain(VertexOut input [[stage_in]]"};

        if (hasUniforms() && fragmentReadsUniforms(graph))
            source += printMetalUniformParameter(",\n    ");

        for (auto i = 0; i < graph.textureCount(); ++i)
            source += Strings::format(",\n    {} texture{} [[texture({})]],\n    "
                                      "sampler sampler{} [[sampler({})]]",
                                      getMetalTextureType(graph.textureKind(i)),
                                      i,
                                      i,
                                      i,
                                      i);

        source += formatBufferParameters(graph, fragmentStatementRoots);
        return source + ")\n{\n";
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

        return "float4 fragmentMain(VertexOut input) : SV_Target\n{\n";
    }

    // The alpha test is planned with the colour so a value both read is shared.
    Vector<int> getColourRoots() const
    {
        auto roots = Vector<int> {graph.fragment()};

        if (graph.discard() >= 0)
            roots.add(graph.discard());

        return roots;
    }

    std::string printFragmentBody() const
    {
        auto stage = StageEmitter {graph, backend, promoted.varyingOf};

        auto source = stage.declareArrays(fragmentStatementRoots, stageIndent);
        source += stage.emitBlock(ShaderGraph::rootBlock, stageIndent);
        source += stage.defineLocalsFor(getColourRoots(), stageIndent);
        source += printAlphaTest(stage);

        auto colour = stage.printer.printReference(graph.fragment());

        if (isGlsl())
            return source
                   + Strings::format("    fragColor = {};\n}}\n#endif\n", colour);

        return source + Strings::format("    return {};\n}}\n", colour);
    }

    std::string printAlphaTest(const StageEmitter& stage) const
    {
        if (graph.discard() < 0)
            return {};

        auto kill = backend == Backend::Metal ? "discard_fragment();" : "discard;";

        return Strings::format("    if ({} < {})\n        {}\n",
                               stage.printer.printReference(graph.discard()),
                               formatShaderFloatLiteral(graph.discardThreshold()),
                               kill);
    }

    const ShaderGraph& graph;
    Backend backend;
    PromotedAttributes promoted;
    Vector<StageVarying> varyings;
    Vector<int> vertexRoots;
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
    return doesAnyReferenceUniform(graph, getVertexStageRoots(graph));
}

bool fragmentReadsUniforms(const ShaderGraph& graph)
{
    return doesAnyReferenceUniform(graph, getFragmentStageRoots(graph));
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
