#include "ShaderFunctionEmission.h"

#include "ShaderDeclarations.h"
#include "ShaderGraphAnalysis.h"
#include "ShaderStageEmitter.h"
#include "ShaderStructEmission.h"

namespace eacp::GPU::ShaderEmission
{
namespace
{
constexpr auto functionBodyIndent = std::string_view {"    "};

Vector<int> getFunctionBodyRoots(const ShaderGraph& graph, int function)
{
    auto roots = Vector<int> {};
    collectStatementExpressions(
        graph, graph.function(function).body, LoopConditions::Included, roots);
    return roots;
}

class CalledFunctionSearch
{
public:
    explicit CalledFunctionSearch(const ShaderGraph& graphToUse)
        : graph(graphToUse)
        , seen(graphToUse.nodeCount())
    {
        functionsSeen.resize(graphToUse.functionCount(), 0);
    }

    void visitNode(int node)
    {
        if (node < 0 || !seen.visit(node))
            return;

        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::FunctionCall)
            visitFunction(expr.index);

        if (expr.kind == ExprKind::ArrayRead)
            for (auto element: graph.arrays()[expr.index].elements)
                visitNode(element);

        for (auto argument: expr.args)
            visitNode(argument);
    }

    Vector<int> order;

private:
    void visitFunction(int function)
    {
        if (functionsSeen[function] != 0)
            return;

        functionsSeen[function] = 1;

        for (auto root: getFunctionBodyRoots(graph, function))
            visitNode(root);

        order.add(function);
    }

    const ShaderGraph& graph;
    VisitSet seen;
    Vector<char> functionsSeen;
};

// What a function's body reaches that MSL holds as an entry point's
// parameter, its callees' included.
struct FunctionResources
{
    explicit FunctionResources(const ShaderGraph& graph)
    {
        textures.resize(graph.textureCount(), 0);
        buffers.resize(graph.storageBuffers().size(), 0);
    }

    bool uniforms = false;
    Vector<char> textures;
    Vector<char> buffers;
};

bool readsTexture(ExprKind kind)
{
    return kind == ExprKind::Sample || kind == ExprKind::Fetch
           || kind == ExprKind::TextureFunction || kind == ExprKind::TextureQuery;
}

bool readsBufferElement(ExprKind kind)
{
    return kind == ExprKind::BufferRead || kind == ExprKind::BufferVectorRead
           || kind == ExprKind::AtomicLoad;
}

class ResourceSearch
{
public:
    ResourceSearch(const ShaderGraph& graphToUse, FunctionResources& found)
        : graph(graphToUse)
        , resources(found)
        , seen(graphToUse.nodeCount())
    {
        functionsSeen.resize(graphToUse.functionCount(), 0);
    }

    void visitFunction(int function)
    {
        if (functionsSeen[function] != 0)
            return;

        functionsSeen[function] = 1;
        visitBlock(graph.function(function).body);

        for (auto root: getFunctionBodyRoots(graph, function))
            visitNode(root);
    }

private:
    void visitBlock(int block)
    {
        for (auto index: graph.block(block).statements)
        {
            const auto& statement = graph.statement(index);
            noteStatement(statement);

            for (auto body:
                 {statement.body, statement.elseBody, statement.continuing})
                if (body >= 0)
                    visitBlock(body);
        }
    }

    void noteStatement(const Statement& statement)
    {
        switch (statement.kind)
        {
            case StatementKind::Store:
            case StatementKind::VectorStore:
                resources.buffers[statement.slot] = 1;
                break;
            case StatementKind::Atomic:
                if (statement.memory == MemorySpace::Buffer)
                    resources.buffers[statement.bufferSlot] = 1;
                break;
            case StatementKind::TextureStore:
                resources.textures[statement.slot] = 1;
                break;
            default:
                break;
        }
    }

    void visitNode(int node)
    {
        if (node < 0 || !seen.visit(node))
            return;

        const auto& expr = graph.expr(node);

        if (expr.kind == ExprKind::Uniform)
            resources.uniforms = true;

        if (readsTexture(expr.kind))
            resources.textures[expr.index] = 1;

        if (expr.kind == ExprKind::TextureFunction && expr.sampler >= 0)
            resources.textures[expr.sampler] = 1;

        if (readsBufferElement(expr.kind))
            resources.buffers[expr.index] = 1;

        if (expr.kind == ExprKind::FunctionCall)
            visitFunction(expr.index);

        for (auto argument: expr.args)
            visitNode(argument);
    }

    const ShaderGraph& graph;
    FunctionResources& resources;
    VisitSet seen;
    Vector<char> functionsSeen;
};

FunctionResources findFunctionResources(const ShaderGraph& graph, int function)
{
    auto resources = FunctionResources {graph};
    auto search = ResourceSearch {graph, resources};
    search.visitFunction(function);
    return resources;
}

// One entry per MSL parameter: its declaration and the name a call passes.
struct MetalResourceParameter
{
    std::string declaration;
    std::string name;
};

void addTextureParameters(const ShaderGraph& graph,
                          int slot,
                          Vector<MetalResourceParameter>& parameters)
{
    auto sampler = Strings::format("sampler{}", slot);
    auto access = graph.textureAccess(slot);

    if (access != TextureAccess::Sampler)
    {
        auto texture = Strings::format("texture{}", slot);
        parameters.add({Strings::format("{} {}",
                                        getMetalTextureParameterType(graph, slot),
                                        texture),
                        texture});
    }

    if (access == TextureAccess::Sampler || access == TextureAccess::Sample)
        parameters.add({"sampler " + sampler, sampler});
}

Vector<MetalResourceParameter> getMetalResourceParameters(const ShaderGraph& graph,
                                                          int function)
{
    auto resources = findFunctionResources(graph, function);
    auto parameters = Vector<MetalResourceParameter> {};

    if (resources.uniforms)
        parameters.add({"constant Uniforms& uniforms", "uniforms"});

    for (auto slot = 0; slot < resources.textures.size(); ++slot)
        if (resources.textures[slot] != 0)
            addTextureParameters(graph, slot, parameters);

    for (auto slot = 0; slot < resources.buffers.size(); ++slot)
    {
        if (resources.buffers[slot] == 0)
            continue;

        auto access =
            graph.isCompute() ? graph.storageBuffers()[slot] : BufferAccess::Read;
        auto name = Strings::format("buffer{}", slot);

        parameters.add({Strings::format("{} {}",
                                        getMetalSlotBufferType(graph, access, slot),
                                        name),
                        name});
    }

    return parameters;
}

std::string printReturnTypeName(const ShaderGraph& graph,
                                Backend backend,
                                const FunctionDeclaration& declaration)
{
    if (!declaration.returnsValue)
        return "void";

    if (declaration.returnType == ValueType::Struct)
        return getStructName(graph, declaration.returnStruct);

    return std::string(getDialectTypeName(backend, declaration.returnType));
}

std::string
    printFunctionDefinition(const ShaderGraph& graph, Backend backend, int function)
{
    const auto& declaration = graph.function(function);
    auto stage = StageEmitter {graph, backend};
    auto parameters = Vector<std::string> {};

    for (auto slot: declaration.parameters)
    {
        stage.markInScope(slot);
        parameters.add(Strings::format(
            "{} v{}", stage.printer.printVariableTypeName(slot), slot));
    }

    if (backend == Backend::Metal)
        for (const auto& resource: getMetalResourceParameters(graph, function))
            parameters.add(resource.declaration);

    auto signature = std::string {};

    for (auto which = 0; which < parameters.size(); ++which)
        signature += (which > 0 ? ", " : "") + parameters[which];

    return Strings::format("{} {}({})\n{{\n{}}}\n\n",
                           printReturnTypeName(graph, backend, declaration),
                           getFunctionName(graph, function),
                           signature,
                           stage.emitBlock(declaration.body, functionBodyIndent));
}
} // namespace

Vector<int> findCalledFunctions(const ShaderGraph& graph, const Vector<int>& roots)
{
    if (graph.functionCount() == 0)
        return {};

    auto search = CalledFunctionSearch {graph};

    for (auto root: roots)
        search.visitNode(root);

    return search.order;
}

void appendFunctionBodyRoots(const ShaderGraph& graph, Vector<int>& roots)
{
    for (auto function: findCalledFunctions(graph, roots))
        collectStatementExpressions(
            graph, graph.function(function).body, LoopConditions::Included, roots);
}

std::string getFunctionName(const ShaderGraph& graph, int function)
{
    return "fn_" + graph.function(function).name;
}

Vector<std::string> getMetalFunctionArguments(const ShaderGraph& graph, int function)
{
    auto names = Vector<std::string> {};

    for (const auto& parameter: getMetalResourceParameters(graph, function))
        names.add(parameter.name);

    return names;
}

std::string printFunctionDefinitions(const ShaderGraph& graph,
                                     Backend backend,
                                     const Vector<int>& functions)
{
    auto source = std::string {};

    for (auto function: functions)
        source += printFunctionDefinition(graph, backend, function);

    return source;
}
} // namespace eacp::GPU::ShaderEmission
