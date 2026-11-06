#include "ShaderStageEmitter.h"

#include "ShaderGroupSpelling.h"

#include <eacp/Core/Utils/Format.h>

namespace eacp::GPU::ShaderEmission
{
namespace
{
constexpr auto nestedIndent = std::string_view {"    "};

std::string printBufferElement(int slot, const std::string& index)
{
    return Strings::format("buffer{}[{}]", slot, index);
}
} // namespace

StageEmitter::StageEmitter(const ShaderGraph& graphToUse,
                           Backend backend,
                           Vector<int> attributeVaryings)
    : printer {graphToUse, backend, locals, std::move(attributeVaryings)}
    , visited(graphToUse.nodeCount())
{
    locals.resize(graphToUse.nodeCount(), -1);
}

std::string StageEmitter::defineLocalsFor(const Vector<int>& roots,
                                          std::string_view indent)
{
    auto open = Vector<int> {};
    return defineLocals(roots, std::string(indent), countUsesOver(roots), open);
}

// Declared before any name exists, in slot order. GLSL drops the const: an
// element read from a uniform is not a constant expression there.
std::string StageEmitter::declareArrays(const Vector<int>& roots,
                                        std::string_view indent)
{
    if (getGraph().arrays().empty())
        return {};

    auto used = findUsedArrays(roots);
    auto source = std::string {};

    for (auto slot = 0; slot < used.size(); ++slot)
        if (used[slot] != 0)
            source += printArrayDeclaration(slot, indent);

    return source;
}

Vector<char> StageEmitter::findUsedArrays(const Vector<int>& roots) const
{
    auto used = Vector<char> {};
    used.resize(getGraph().arrays().size(), 0);
    auto seen = Vector<char> {};
    seen.resize(getGraph().nodeCount(), 0);

    for (auto root: roots)
        collectArrays(getGraph(), root, used, seen);

    return used;
}

std::string StageEmitter::printArrayDeclaration(int slot,
                                                std::string_view indent) const
{
    const auto& array = getGraph().arrays()[slot];
    auto qualifier = printer.backend == Backend::Vulkan ? "" : "const ";

    auto source =
        Strings::format("{}{}{} a{}[{}] = {{",
                        indent,
                        qualifier,
                        getDialectTypeName(printer.backend, array.elementType),
                        slot,
                        array.elements.size());

    for (auto i = 0; i < array.elements.size(); ++i)
    {
        if (i > 0)
            source += ", ";

        source += printer.printReference(array.elements[i]);
    }

    return source + "};\n";
}

std::string StageEmitter::emitBlock(int block, std::string_view indentToUse)
{
    const auto indent = std::string(indentToUse);
    auto uses = countBlockUses(block);
    auto open = Vector<int> {};
    auto source = std::string {};

    for (auto index: getGraph().block(block).statements)
        source += emitStatement(getGraph().statement(index), indent, uses, open);

    retireLocals(open);
    return source;
}

StageEmitter::StatementEmitFunction
    StageEmitter::findStatementEmitter(StatementKind kind)
{
    static constexpr auto emitters = Array<StatementEmitEntry, 16> {
        {StatementKind::Declare, &StageEmitter::emitVariableWrite},
        {StatementKind::Assign, &StageEmitter::emitVariableWrite},
        {StatementKind::If, &StageEmitter::emitIf},
        {StatementKind::Break, &StageEmitter::emitBreak},
        {StatementKind::Continue, &StageEmitter::emitContinue},
        {StatementKind::Store, &StageEmitter::emitStore},
        {StatementKind::VectorStore, &StageEmitter::emitVectorStore},
        {StatementKind::AtomicAdd, &StageEmitter::emitAtomicAdd},
        {StatementKind::SharedStore, &StageEmitter::emitSharedStore},
        {StatementKind::Barrier, &StageEmitter::emitBarrier},
        {StatementKind::GroupReduce, &StageEmitter::emitGroupReduce},
        {StatementKind::SimdMatrixFill, &StageEmitter::emitSimdMatrixFill},
        {StatementKind::SimdMatrixLoad, &StageEmitter::emitSimdMatrixTransfer},
        {StatementKind::SimdMatrixStore, &StageEmitter::emitSimdMatrixTransfer},
        {StatementKind::SimdMatrixMultiplyAdd,
         &StageEmitter::emitSimdMatrixMultiplyAdd},
        {StatementKind::TextureStore, &StageEmitter::emitTextureStore},
    };

    for (const auto& entry: emitters)
        if (entry.kind == kind)
            return entry.emit;

    return &StageEmitter::emitNothing;
}

std::string StageEmitter::emitStatement(const Statement& statement,
                                        const std::string& indent,
                                        const Vector<int>& uses,
                                        Vector<int>& open)
{
    if (statement.kind == StatementKind::Loop)
        return emitLoop(statement, indent, open);

    auto scope = StatementScope {indent, uses, open};
    auto source = (this->*findStatementEmitter(statement.kind))(statement, scope);

    dropStaleLocals(statement, open);
    return source;
}

std::string StageEmitter::emitLoop(const Statement& statement,
                                   const std::string& indent,
                                   Vector<int>& open)
{
    dropStaleLocals(statement, open);

    return Strings::format(
               "{}while ({})\n", indent, printer.printReference(statement.value))
           + printBody(statement.body, indent);
}

std::string StageEmitter::printBody(int block, const std::string& indent)
{
    return Strings::format(
        "{}{{\n{}{}}}\n",
        indent,
        emitBlock(block, Strings::format("{}{}", indent, nestedIndent)),
        indent);
}

std::string StageEmitter::defineLocalsInScope(const Vector<int>& roots,
                                              StatementScope& scope)
{
    return defineLocals(roots, scope.indent, scope.uses, scope.open);
}

std::string StageEmitter::emitVariableWrite(const Statement& statement,
                                            StatementScope& scope)
{
    auto type = std::string {};

    if (statement.kind == StatementKind::Declare)
        type = Strings::format(
            "{} ",
            getDialectTypeName(printer.backend,
                               getGraph().variables()[statement.slot]));

    auto source = defineLocalsInScope({statement.value}, scope);
    source += Strings::format("{}{}v{} = {};\n",
                              scope.indent,
                              type,
                              statement.slot,
                              printer.printReference(statement.value));
    return source;
}

std::string StageEmitter::emitIf(const Statement& statement, StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.value}, scope);

    auto condition = printer.printReference(statement.value);
    dropStaleLocals(statement, scope.open);

    source += Strings::format("{}if ({})\n", scope.indent, condition);
    source += printBody(statement.body, scope.indent);

    if (statement.elseBody >= 0)
    {
        source += scope.indent + "else\n";
        source += printBody(statement.elseBody, scope.indent);
    }

    return source;
}

std::string StageEmitter::emitBreak(const Statement&, StatementScope& scope)
{
    return scope.indent + "break;\n";
}

std::string StageEmitter::emitContinue(const Statement&, StatementScope& scope)
{
    return scope.indent + "continue;\n";
}

std::string StageEmitter::emitStore(const Statement& statement,
                                    StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.index, statement.value}, scope);
    source += holdTheRecord(statement, scope.indent, scope.open);

    auto element =
        printBufferElement(statement.slot, printer.printReference(statement.index));
    auto stored = printer.printReference(statement.value);
    auto atomic =
        getGraph().storageBuffers()[statement.slot] == BufferAccess::Atomic;

    if (atomic && printer.backend == Backend::Metal)
        source += Strings::format(
            "{}atomic_store_explicit(&{}, {}, memory_order_relaxed);\n",
            scope.indent,
            element,
            stored);
    else
        source += Strings::format("{}{} = {};\n", scope.indent, element, stored);

    return source;
}

// Both operands are named first, so write4(out, i, f(out.read4(i))) reads
// before any component lands.
std::string StageEmitter::emitVectorStore(const Statement& statement,
                                          StatementScope& scope)
{
    const auto& indent = scope.indent;
    auto source = defineLocalsInScope({statement.index, statement.value}, scope);
    source += holdTheVector(statement, indent, scope.open);

    auto base = printer.printReference(statement.index);
    auto stored = printer.printReference(statement.value);
    auto type = getGraph().expr(statement.value).type;

    if (printer.backend == Backend::Metal)
        return source
               + Strings::format("{}*((device {}*) (buffer{} + {})) = {};\n",
                                 indent,
                                 getMetalPackedVectorType(type),
                                 statement.slot,
                                 base,
                                 stored);

    for (auto component = 0; component < componentCount(type); ++component)
    {
        source += Strings::format("{}buffer{}[{}", indent, statement.slot, base);

        if (component > 0)
            source += Strings::format(" + {}u", component);

        source += Strings::format(
            "] = ({}){};\n", stored, getVectorComponentSuffix(component));
    }

    return source;
}

// HLSL's InterlockedAdd returns the old value through an out parameter.
std::string StageEmitter::emitAtomicAdd(const Statement& statement,
                                        StatementScope& scope)
{
    const auto& indent = scope.indent;
    auto source = defineLocalsInScope({statement.index, statement.value}, scope);

    auto name = Strings::format("v{}", statement.slot);
    auto element = printBufferElement(statement.bufferSlot,
                                      printer.printReference(statement.index));
    auto addend = printer.printReference(statement.value);

    if (printer.backend == Backend::Metal)
        return source
               + Strings::format("{}uint {} = atomic_fetch_add_explicit(&{}, {}, "
                                 "memory_order_relaxed);\n",
                                 indent,
                                 name,
                                 element,
                                 addend);

    if (printer.backend == Backend::Vulkan)
        return source
               + Strings::format("{}uint {} = atomicAdd({}, {});\n",
                                 indent,
                                 name,
                                 element,
                                 addend);

    source += Strings::format("{}uint {};\n", indent, name);
    source += Strings::format(
        "{}InterlockedAdd({}, {}, {});\n", indent, element, addend, name);
    return source;
}

std::string StageEmitter::emitSharedStore(const Statement& statement,
                                          StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.index, statement.value}, scope);
    source += Strings::format("{}s{}[{}] = {};\n",
                              scope.indent,
                              statement.slot,
                              printer.printReference(statement.index),
                              printer.printReference(statement.value));
    return source;
}

std::string StageEmitter::emitBarrier(const Statement&, StatementScope& scope)
{
    return formatBarrierStatement(printer.backend, scope.indent);
}

std::string StageEmitter::emitGroupReduce(const Statement& statement,
                                          StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.value}, scope);
    return source + formatGroupReduction(statement, scope);
}

std::string StageEmitter::emitSimdMatrixFill(const Statement& statement,
                                             StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.value}, scope);
    return source + formatSimdMatrixFill(statement, scope);
}

std::string StageEmitter::emitSimdMatrixTransfer(const Statement& statement,
                                                 StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.index, statement.stride}, scope);
    return source + formatSimdMatrixTransfer(statement, scope);
}

std::string StageEmitter::emitSimdMatrixMultiplyAdd(const Statement& statement,
                                                    StatementScope& scope)
{
    return formatSimdMatrixMultiplyAdd(statement, scope);
}

// GLSL's imageStore takes a signed coordinate.
std::string StageEmitter::emitTextureStore(const Statement& statement,
                                           StatementScope& scope)
{
    const auto& indent = scope.indent;
    auto source = defineLocalsInScope(
        {statement.index, statement.indexY, statement.value}, scope);

    auto pair = printer.backend == Backend::Vulkan ? "ivec2" : "uint2";
    auto coordinates = Strings::format("{}({}, {})",
                                       pair,
                                       printer.printReference(statement.index),
                                       printer.printReference(statement.indexY));
    auto color = printer.printReference(statement.value);

    if (printer.backend == Backend::Metal)
        return source
               + Strings::format("{}texture{}.write({}, {});\n",
                                 indent,
                                 statement.slot,
                                 color,
                                 coordinates);

    if (printer.backend == Backend::Vulkan)
        return source
               + Strings::format("{}imageStore(texture{}, {}, {});\n",
                                 indent,
                                 statement.slot,
                                 coordinates,
                                 color);

    return source
           + Strings::format("{}texture{}[{}] = {};\n",
                             indent,
                             statement.slot,
                             coordinates,
                             color);
}

std::string StageEmitter::emitNothing(const Statement&, StatementScope&)
{
    return {};
}

Vector<int> StageEmitter::countUsesOver(const Vector<int>& roots) const
{
    auto count = getGraph().nodeCount();
    auto uses = Vector<int> {};
    auto seen = Vector<char> {};

    uses.resize(count, 0);
    seen.resize(count, 0);

    for (auto root: roots)
        countUses(getGraph(), root, uses, seen);

    return uses;
}

Vector<int> StageEmitter::countBlockUses(int block) const
{
    auto roots = Vector<int> {};
    collectStatementExpressions(getGraph(), block, LoopConditions::Excluded, roots);
    return countUsesOver(roots);
}

std::string StageEmitter::defineLocals(const Vector<int>& roots,
                                       const std::string& indent,
                                       const Vector<int>& uses,
                                       Vector<int>& open)
{
    auto ordered = Vector<char> {};
    ordered.resize(getGraph().nodeCount(), 0);
    auto order = Vector<int> {};

    auto ordering = LocalOrdering {getGraph(), uses, locals, ordered, order};

    for (auto root: roots)
        ordering.visit(root);

    auto source = std::string {};

    for (auto node: order)
        source += bindLocal(node, indent, open);

    return source;
}

std::string
    StageEmitter::bindLocal(int node, const std::string& indent, Vector<int>& open)
{
    locals[node] = localCount++;
    open.add(node);

    return Strings::format(
        "{}{} t{} = {};\n",
        indent,
        getDialectTypeName(printer.backend, getGraph().expr(node).type),
        locals[node],
        printer.print(node));
}

bool StageEmitter::isRecordStillUnnamed(const Statement& statement) const
{
    return statement.recordComponentsLeft > 0 && locals[statement.record] < 0;
}

std::string StageEmitter::holdTheRecord(const Statement& statement,
                                        const std::string& indent,
                                        Vector<int>& open)
{
    if (!isRecordStillUnnamed(statement))
        return {};

    if (!doesReadSlot(statement.record, statement.slot))
        return {};

    return bindLocal(statement.record, indent, open);
}

// HLSL and GLSL print the index into every subscript, so an index that reads
// the buffer being written would see this store's own earlier components.
std::string StageEmitter::holdTheVector(const Statement& statement,
                                        const std::string& indent,
                                        Vector<int>& open)
{
    auto source = std::string {};

    if (printer.backend != Backend::Metal)
        source += holdOperand(statement.index, statement.slot, indent, open);

    return source + holdOperand(statement.value, statement.slot, indent, open);
}

// A leaf is repeated freely unless it reads the buffer being written.
std::string StageEmitter::holdOperand(int node,
                                      int slot,
                                      const std::string& indent,
                                      Vector<int>& open)
{
    if (node < 0 || locals[node] >= 0)
        return {};

    if (!isLocalWanted(getGraph().expr(node).kind) && !doesReadSlot(node, slot))
        return {};

    return bindLocal(node, indent, open);
}

bool StageEmitter::doesReadSlot(int node, int slot)
{
    written.assign(getGraph().variables().size(), char {0});
    buffersWritten.assign(getGraph().storageBuffers().size(), char {0});
    buffersWritten[slot] = 1;
    visited.restart();

    auto query =
        StalenessQuery {getGraph(), written, buffersWritten, false, visited};
    return query.doesReadStaleValue(node);
}

void StageEmitter::retireLocals(Vector<int>& open)
{
    for (auto node: open)
        locals[node] = -1;

    open.clear();
}

bool StageEmitter::hasNothingStale(bool sharedMoved) const
{
    return !written.contains(1) && !buffersWritten.contains(1) && !sharedMoved;
}

void StageEmitter::dropStaleLocals(const Statement& statement, Vector<int>& open)
{
    if (open.empty())
        return;

    auto sharedMoved = doesTouchSharedMemory(getGraph(), statement);

    written.assign(getGraph().variables().size(), char {0});
    collectWrites(getGraph(), statement, written);

    buffersWritten.assign(getGraph().storageBuffers().size(), char {0});
    collectBufferWrites(getGraph(), statement, buffersWritten);

    if (hasNothingStale(sharedMoved))
        return;

    auto heldRecord = statement.recordComponentsLeft > 0 ? statement.record : -1;
    auto query =
        StalenessQuery {getGraph(), written, buffersWritten, sharedMoved, visited};
    auto kept = Vector<int> {};

    for (auto node: open)
    {
        visited.restart();

        if (node != heldRecord && query.doesReadStaleValue(node))
            locals[node] = -1;
        else
            kept.add(node);
    }

    open = std::move(kept);
}
} // namespace eacp::GPU::ShaderEmission
