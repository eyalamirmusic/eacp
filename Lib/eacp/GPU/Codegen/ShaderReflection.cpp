#include "ShaderReflection.h"

#include "ShaderDialect.h"
#include "ShaderEmitter.h"
#include "ShaderFunctionEmission.h"
#include "ShaderGraphAnalysis.h"
#include "UniformLayout.h"

namespace eacp::GPU
{
namespace
{
using namespace ShaderEmission;

PipelineConstantType reflectedConstantType(ValueType type)
{
    switch (type)
    {
        case ValueType::Bool:
            return PipelineConstantType::Bool;
        case ValueType::Int:
            return PipelineConstantType::Int;
        case ValueType::UInt:
            return PipelineConstantType::UInt;
        case ValueType::Half:
            return PipelineConstantType::Half;
        default:
            return PipelineConstantType::Float;
    }
}

// Which slots one render stage reads: buffers by buffer slot, textures and
// samplers by texture slot, and which functions it calls.
struct StageReach
{
    StageReach(const ShaderGraph& graph, const Vector<int>& nodes)
    {
        buffers.resize(graph.storageBuffers().size(), 0);
        textures.resize(graph.textureCount(), 0);
        functions.resize(graph.functionCount(), 0);

        for (auto node: nodes)
            visit(graph.expr(node));
    }

    void visit(const Expr& expr)
    {
        switch (expr.kind)
        {
            case ExprKind::BufferRead:
            case ExprKind::BufferVectorRead:
            case ExprKind::AtomicLoad:
                buffers[expr.index] = 1;
                break;

            case ExprKind::Sample:
            case ExprKind::Fetch:
            case ExprKind::TextureFunction:
            case ExprKind::TextureQuery:
                textures[expr.index] = 1;
                break;

            case ExprKind::FunctionCall:
                functions[expr.index] = 1;
                break;

            default:
                break;
        }

        if (expr.kind == ExprKind::TextureFunction && expr.sampler >= 0)
            textures[expr.sampler] = 1;
    }

    Vector<char> buffers;
    Vector<char> textures;
    Vector<char> functions;
};

struct StageReaches
{
    explicit StageReaches(const ShaderGraph& graph)
        : vertex(graph, graph.isCompute() ? Vector<int> {} : vertexStageNodes(graph))
        , fragment(graph,
                   graph.isCompute() ? Vector<int> {} : fragmentStageNodes(graph))
    {
    }

    StageVisibility buffer(const ShaderGraph& graph, int slot) const
    {
        if (graph.isCompute())
            return {.compute = true};

        return {.vertex = vertex.buffers[slot] != 0,
                .fragment = fragment.buffers[slot] != 0};
    }

    StageVisibility texture(const ShaderGraph& graph, int slot) const
    {
        if (graph.isCompute())
            return {.compute = true};

        return {.vertex = vertex.textures[slot] != 0,
                .fragment = fragment.textures[slot] != 0};
    }

    StageReach vertex;
    StageReach fragment;
};

ShaderBinding reflectUniformBlock(const ShaderGraph& graph)
{
    auto binding = ShaderBinding {};
    binding.kind = BindingKind::UniformBlock;

    if (graph.isCompute())
        binding.stages.compute = true;
    else
        binding.stages = {.vertex = vertexReadsUniforms(graph),
                          .fragment = fragmentReadsUniforms(graph)};

    return binding;
}

ShaderBinding reflectStorageBuffer(const ShaderGraph& graph,
                                   const StageReaches& reaches,
                                   int slot)
{
    auto binding = ShaderBinding {};
    binding.kind = BindingKind::StorageBuffer;
    binding.slot = slot;
    binding.name = graph.storageBufferName(slot);
    binding.stages = reaches.buffer(graph, slot);
    binding.access = graph.storageBuffers()[slot];
    binding.elementType = graph.storageElementType(slot);
    binding.structType = graph.storageStructType(slot);
    return binding;
}

BindingKind textureBindingKind(const ShaderGraph& graph, int slot)
{
    const auto access = graph.textureAccess(slot);

    if (access == TextureAccess::Sampler)
        return graph.isComparisonSampler(slot) ? BindingKind::ComparisonSampler
                                               : BindingKind::Sampler;

    return isStorageAccess(access) ? BindingKind::StorageTexture
                                   : BindingKind::Texture;
}

ShaderBinding reflectTextureSlot(const ShaderGraph& graph,
                                 const StageReaches& reaches,
                                 int slot)
{
    auto binding = ShaderBinding {};
    binding.kind = textureBindingKind(graph, slot);
    binding.slot = slot;
    binding.name = graph.textureName(slot);
    binding.stages = reaches.texture(graph, slot);
    binding.textureAccess = graph.textureAccess(slot);

    if (binding.kind == BindingKind::Texture
        || binding.kind == BindingKind::StorageTexture)
    {
        binding.textureKind = graph.textureKind(slot);
        binding.sampling = graph.textureSampling(slot);
        binding.format = graph.textureFormat(slot);
    }

    return binding;
}

void reflectUniformMembers(const ShaderGraph& graph, ShaderReflection& reflection)
{
    auto types = graph.uniforms();
    const auto declared = types.size();

    if (graph.isCompute())
        for (auto component = 0;
             component < getGridExtentCount(graph.dispatchRank());
             ++component)
            types.add(ValueType::UInt);

    const auto offsets = uniformOffsets(types);

    for (auto index = 0; index < types.size(); ++index)
    {
        auto member = UniformMember {};
        member.type = types[index];
        member.offset = offsets[index];
        member.size = uniformSlotStride(types[index]);
        member.gridExtent = index >= declared;

        if (!member.gridExtent)
            member.name = graph.uniformName(index);
        else if (graph.dispatchRank() == DispatchRank::OneD)
            member.name = "count";
        else
            member.name = std::string(getGridExtentName(index - declared));

        reflection.uniformMembers.add(member);
    }

    reflection.uniformBlockSize = types.empty() ? 0 : uniformBlockSize(types);
}

void reflectVertexInterface(const ShaderGraph& graph, ShaderReflection& reflection)
{
    for (auto location = 0; location < graph.inputs().size(); ++location)
        reflection.vertexInputs.add({location,
                                     graph.inputs()[location],
                                     graph.inputStepRates()[location],
                                     graph.inputBufferIndices()[location]});

    const auto varyings = emittedVaryings(graph);

    for (auto location = 0; location < varyings.size(); ++location)
    {
        const auto& varying = varyings[location];
        reflection.varyings.add(
            {location, varying.type, varying.interpolation, varying.attribute});
    }
}

void reflectOverrides(const ShaderGraph& graph, ShaderReflection& reflection)
{
    for (auto id = 0; id < graph.overrides().size(); ++id)
    {
        const auto& declared = graph.overrides()[id];

        auto reflected = OverrideReflection {};
        reflected.id = id;
        reflected.name = declared.name;
        reflected.type = declared.type;
        reflected.hasDefault = declared.hasDefault;
        reflected.defaultValue.id = id;
        reflected.defaultValue.type = reflectedConstantType(declared.type);
        reflected.defaultValue.bits = declared.defaultBits;
        reflection.overrides.add(reflected);
    }
}

Vector<char> kernelCalledFunctions(const ShaderGraph& graph)
{
    auto called = Vector<char> {};
    called.resize(graph.functionCount(), 0);

    auto roots = Vector<int> {};
    collectStatementExpressions(
        graph, ShaderGraph::rootBlock, LoopConditions::Included, roots);

    for (auto function: findCalledFunctions(graph, roots))
        called[function] = 1;

    return called;
}

void reflectFunctions(const ShaderGraph& graph,
                      const StageReaches& reaches,
                      ShaderReflection& reflection)
{
    const auto kernelCalls =
        graph.isCompute() ? kernelCalledFunctions(graph) : Vector<char> {};

    for (auto id = 0; id < graph.functionCount(); ++id)
    {
        const auto& declared = graph.function(id);

        auto reflected = FunctionReflection {};
        reflected.name = declared.name;

        for (auto parameter: declared.parameters)
            reflected.parameters.add(
                {graph.variables()[parameter], graph.variableStructType(parameter)});

        reflected.returnsValue = declared.returnsValue;
        reflected.returnType = declared.returnType;
        reflected.returnStruct = declared.returnStruct;
        reflected.calledAsStatement = graph.isCalledAsStatement(id);
        reflected.writesStorage = declared.writesStorage;
        reflected.discards = declared.discards;
        reflected.isPure = declared.isPure;

        if (graph.isCompute())
            reflected.calledFrom.compute = kernelCalls[id] != 0;
        else
            reflected.calledFrom = {.vertex = reaches.vertex.functions[id] != 0,
                                    .fragment = reaches.fragment.functions[id] != 0};

        reflection.functions.add(reflected);
    }
}

void reflectStructs(const ShaderGraph& graph, ShaderReflection& reflection)
{
    for (auto id = 0; id < graph.structCount(); ++id)
    {
        const auto& declared = graph.structType(id);
        reflection.structs.add({declared.name,
                                declared.members,
                                declared.size,
                                declared.alignment,
                                declared.hostShareable});
    }
}
} // namespace

bool StageVisibility::includes(ShaderStage stage) const
{
    switch (stage)
    {
        case ShaderStage::Vertex:
            return vertex;
        case ShaderStage::Fragment:
            return fragment;
        case ShaderStage::Compute:
            break;
    }

    return compute;
}

bool StageVisibility::isEmpty() const
{
    return !vertex && !fragment && !compute;
}

std::string_view bindingKindName(BindingKind kind)
{
    switch (kind)
    {
        case BindingKind::UniformBlock:
            return "uniform block";
        case BindingKind::StorageBuffer:
            return "storage buffer";
        case BindingKind::Texture:
            return "texture";
        case BindingKind::StorageTexture:
            return "storage texture";
        case BindingKind::Sampler:
            return "sampler";
        case BindingKind::ComparisonSampler:
            break;
    }

    return "comparison sampler";
}

bool ShaderBinding::isTextureSlot() const
{
    return kind != BindingKind::UniformBlock && kind != BindingKind::StorageBuffer;
}

const ShaderBinding* ShaderReflection::findBinding(BindingKind kind, int slot) const
{
    for (const auto& binding: bindings)
        if (binding.kind == kind && binding.slot == slot)
            return &binding;

    return nullptr;
}

const ShaderBinding* ShaderReflection::findBinding(std::string_view name) const
{
    for (const auto& binding: bindings)
        if (!binding.name.empty() && binding.name == name)
            return &binding;

    return nullptr;
}

const OverrideReflection* ShaderReflection::findOverride(std::string_view name) const
{
    for (const auto& reflected: overrides)
        if (reflected.name == name)
            return &reflected;

    return nullptr;
}

const FunctionReflection* ShaderReflection::findFunction(std::string_view name) const
{
    for (const auto& reflected: functions)
        if (reflected.name == name)
            return &reflected;

    return nullptr;
}

ShaderReflection reflectShader(const ShaderGraph& graph)
{
    auto reflection = ShaderReflection {};
    const auto reaches = StageReaches {graph};

    reflection.isCompute = graph.isCompute();

    if (graph.isCompute() || !graph.uniforms().empty())
        reflection.bindings.add(reflectUniformBlock(graph));

    for (auto slot = 0; slot < graph.storageBuffers().size(); ++slot)
        reflection.bindings.add(reflectStorageBuffer(graph, reaches, slot));

    for (auto slot = 0; slot < graph.textureCount(); ++slot)
        reflection.bindings.add(reflectTextureSlot(graph, reaches, slot));

    reflectUniformMembers(graph, reflection);

    if (!graph.isCompute())
        reflectVertexInterface(graph, reflection);

    reflectOverrides(graph, reflection);
    reflectFunctions(graph, reaches, reflection);
    reflectStructs(graph, reflection);

    if (graph.isCompute())
    {
        reflection.dispatchRank = graph.dispatchRank();
        reflection.workgroupSize = graph.threadGroupShape();
        reflection.threadgroupMemoryBytes = graph.threadgroupMemoryBytes();
    }

    reflection.usesHalf = graph.usesHalf();
    reflection.usesSampleRateShading = graph.usesSampleRateShading();
    return reflection;
}
} // namespace eacp::GPU
