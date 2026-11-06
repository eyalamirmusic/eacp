#include "ShaderEmitter.h"

#include "../Frame/RenderPass.h"
#include "ShaderBindings.h"
#include "ShaderComputeEmitter.h"
#include "ShaderDeclarations.h"
#include "ShaderHelpers.h"
#include "ShaderStageEmitter.h"

#include <eacp/Core/Utils/Format.h>

// The single source-of-truth walker. A render graph emits as a vertex and a
// fragment function in one source; a compute graph as one kernel
// (ShaderComputeEmitter.cpp). Both stages share the expression printer and the
// statement emitter, so the three dialects stay in lockstep by construction.

namespace eacp::GPU
{
namespace
{
using namespace ShaderEmission;

constexpr auto stageIndent = "    ";

class RenderShaderEmitter
{
public:
    RenderShaderEmitter(const ShaderGraph& graphToUse, Backend backendToUse)
        : graph(graphToUse)
        , backend(backendToUse)
        , promoted(promotedAttributes(graphToUse))
        , varyings(stageVaryings(graphToUse, promoted))
        , vertexRoots(vertexStageRoots(graphToUse))
        , fragmentStatementRoots(fragmentStageRoots(graphToUse))
    {
    }

    std::string emit() const
    {
        auto source = sourcePreamble(graph, backend);

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
                                      typeName(backend, graph.inputs()[i]),
                                      i,
                                      attributeSemantic(backend, i));

        source += "};\n\nstruct VertexOut\n{\n";
        source +=
            Strings::format("    float4 position{};\n", positionSemantic(backend));

        for (auto i = 0; i < varyings.size(); ++i)
        {
            auto type = varyings[i].type;

            source += Strings::format("    {}{} v{}{};\n",
                                      flatQualifier(backend, type),
                                      typeName(backend, type),
                                      i,
                                      varyingSemantic(backend, i, type));
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

        return uniformBlock(backend, graph.uniforms(), names);
    }

    // Read-only whatever the slot recorded: a render stage never writes.
    std::string printGlslBindings() const
    {
        auto source = std::string {};

        for (auto i = 0; i < graph.textureCount(); ++i)
            source += glslSampledTexture(graph, i, vulkanTextureBinding(i));

        if (graph.textureCount() > 0)
            source += "\n";

        for (auto i = 0; i < graph.storageBuffers().size(); ++i)
            source += glslBufferBlock(BufferAccess::Read,
                                      graph.storageElementType(i),
                                      i,
                                      vulkanBufferBinding(i));

        if (graph.storageBuffers().size() > 0)
            source += "\n";

        return source;
    }

    // The texture lands on t<slot>. Its sampler does not land beside it: the
    // root signature declares one static sampler per sampling configuration and
    // every texture that declared that sampling reads the same one, which is
    // what keeps a slot count from costing sampler registers. See
    // TextureSampling, and hlslSamplerDeclarations.
    //
    // Storage buffers are globals here like the textures, at registers above
    // every texture slot - the render signature's mirror of the way a kernel's
    // textures sit above its buffers. See RenderPass::bufferRegisterBase.
    std::string printHlslBindings() const
    {
        auto source = std::string {};

        for (auto i = 0; i < graph.textureCount(); ++i)
            source += Strings::format("{} texture{} : register(t{});\n",
                                      hlslTextureType(graph.textureKind(i)),
                                      i,
                                      i);

        source += hlslSamplerDeclarations(graph);

        if (graph.textureCount() > 0)
            source += "\n";

        for (auto i = 0; i < graph.storageBuffers().size(); ++i)
            source += Strings::format(
                "{} buffer{} : register(t{});\n",
                hlslBufferType(BufferAccess::Read, graph.storageElementType(i)),
                i,
                RenderPass::bufferRegisterBase + i);

        if (graph.storageBuffers().size() > 0)
            source += "\n";

        return source;
    }

    // The varyings as one GLSL stage declares them: out of the vertex stage,
    // into the fragment stage.
    std::string printGlslVaryings(const char* direction) const
    {
        auto source = std::string {};

        for (auto i = 0; i < varyings.size(); ++i)
        {
            auto type = varyings[i].type;

            source += Strings::format("{}{}{} {} vary{};\n",
                                      locationLayout(i),
                                      flatQualifier(backend, type),
                                      direction,
                                      typeName(backend, type),
                                      i);
        }

        return source;
    }

    std::string printMetalUniformParameter(const char* separator) const
    {
        return Strings::format("{}constant Uniforms& uniforms [[buffer({})]]",
                               separator,
                               RenderPass::uniformBase);
    }

    // On Metal each stage declares the uniform block as a function parameter,
    // and only when that stage's expressions read one; the HLSL cbuffer is a
    // global both functions already see. Slot 0 maps to buffer(uniformBase) in
    // both stages, matching what RenderPass::setVertexBytes / setFragmentBytes
    // bind. Uniforms live at buffer(uniformBase..) so a vertex layout with
    // multiple per-instance slots (0..N) never collides with them.
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

        source += bufferParameters(graph, vertexRoots);
        return source + ")\n{\n";
    }

    std::string printGlslVertexOpening() const
    {
        auto source = std::string {"#ifdef EACP_VERTEX\n"};

        for (auto i = 0; i < graph.inputs().size(); ++i)
            source += Strings::format("{}in {} attr{};\n",
                                      locationLayout(i),
                                      typeName(backend, graph.inputs()[i]),
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

    // The vertex stage takes no statements: a mutable local and the control
    // flow driving it belong to the fragment expression, the way sampling does.
    // See ShaderBuilder::var.
    //
    // Clip y is left as the graph computed it: Vulkan's flipped NDC is a
    // negative viewport height in RenderPass, not a negation here, which flips
    // winding.
    std::string printVertexBody() const
    {
        auto stage = StageEmitter {graph, backend};

        auto source = stage.declareArrays(vertexRoots, stageIndent);
        source += stage.defineFor(vertexRoots, stageIndent);

        auto position = stage.printer.ref(graph.position());

        if (isGlsl())
            return source + Strings::format("    gl_Position = {};\n", position)
                   + printVaryingAssignments(stage, "vary") + "}\n#endif\n\n";

        source += "    VertexOut output;\n";
        source += Strings::format("    output.position = {};\n", position);
        source += printVaryingAssignments(stage, "output.v");
        return source + "    return output;\n}\n\n";
    }

    std::string printVaryingAssignments(const StageEmitter& stage,
                                        const char* prefix) const
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
            return stage.printer.ref(varying.sourceNode);

        return attributeName(backend, varying.attribute);
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
                                      metalTextureType(graph.textureKind(i)),
                                      i,
                                      i,
                                      i,
                                      i);

        source += bufferParameters(graph, fragmentStatementRoots);
        return source + ")\n{\n";
    }

    std::string printGlslFragmentOpening() const
    {
        auto source = std::string {"#ifdef EACP_FRAGMENT\n"};
        source += printGlslVaryings("in");
        source += locationLayout(0) + "out vec4 fragColor;\n";
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

    // The alpha test is a fragment root like the colour is: its subtree is
    // planned with the colour's, so a value both of them read (the texture
    // sample, typically) is computed once and shared. What the statements read
    // counts towards the stage's uniform declaration, but not towards the
    // colour's locals: a statement's own expressions are named where that
    // statement is emitted.
    Vector<int> colourRoots() const
    {
        auto roots = Vector<int> {graph.fragment()};

        if (graph.discard() >= 0)
            roots.add(graph.discard());

        return roots;
    }

    // The shader's statements run first - they are what the fragment expression
    // then reads a mutable local out of - and the colour is planned after them.
    std::string printFragmentBody() const
    {
        auto stage = StageEmitter {graph, backend, promoted.varyingOf};

        auto source = stage.declareArrays(fragmentStatementRoots, stageIndent);
        source += stage.emitBlock(ShaderGraph::rootBlock, stageIndent);
        source += stage.defineFor(colourRoots(), stageIndent);
        source += printAlphaTest(stage);

        auto colour = stage.printer.ref(graph.fragment());

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
                               stage.printer.ref(graph.discard()),
                               floatLiteral(graph.discardThreshold()),
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
