#include "ShaderStageEmitter.h"

#include "ShaderGroupSpelling.h"

#include <eacp/Core/Utils/Format.h>

namespace eacp::GPU::ShaderEmission
{
namespace
{
constexpr auto nestedIndent = "    ";

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

// The locals a standalone run of expressions needs - a stage's outputs, which
// no statement follows - counted over just those expressions.
std::string StageEmitter::defineLocalsFor(const Vector<int>& roots,
                                          const std::string& indent)
{
    auto open = Vector<int> {};
    return defineLocals(roots, indent, countUsesOver(roots), open);
}

// The constant arrays a stage subscripts, declared at the top of its function,
// before any name has been handed out - so an element may read a uniform or a
// varying but not a mutable local.
//
// The GLSL form drops the const: an element read from a uniform is not a
// constant expression, which is all GLSL lets one initialise a const with.
//
// Emitted in slot order, so an array whose elements read another one finds it
// already there.
std::string StageEmitter::declareArrays(const Vector<int>& roots,
                                        const std::string& indent)
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
                                                const std::string& indent) const
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

std::string StageEmitter::emitBlock(int block, const std::string& indent)
{
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
    static constexpr StatementEmitEntry emitters[] = {
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

// Afterwards either way, for the names a statement's own expressions
// introduced: a value read out of the variable it then wrote.
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
    return indent + "{\n" + emitBlock(block, indent + nestedIndent) + indent + "}\n";
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
        type = std::string(getDialectTypeName(
                   printer.backend, getGraph().variables()[statement.slot]))
               + " ";

    auto source = defineLocalsInScope({statement.value}, scope);
    source += Strings::format("{}{}v{} = {};\n",
                              scope.indent,
                              type,
                              statement.slot,
                              printer.printReference(statement.value));
    return source;
}

// The condition is evaluated before either body runs, so it is printed while
// every name still stands; the ones a body moves on from are given up between
// it and them. An assignment needs no such pass first: its right-hand side is
// what the variable held before it, which is what the open names still stand
// for.
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

// An atomic buffer's element is an atomic_uint on Metal and has to be stored
// through rather than assigned. HLSL's UAV element is an ordinary uint, so the
// plain assignment is already right there.
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

// The write mirror of a vector read: one store where the dialect has a
// spelling for one, and the N subscripts it stands in for where it has not.
// Metal reinterprets the address being stored to rather than the binding, so an
// output stays a run of floats and nothing it was bindable as is given up.
//
// Both operands are named first - see holdTheVector - so each is evaluated once
// and in full before any part of the record reaches memory. That is what lets
// write4(out, i, f(out.read4(i))) mean what it says, and what keeps an index
// computed from the buffer being written - write4(out, toUInt(out[i]), v) -
// addressing the element it was aimed at rather than the one the first
// component just landed on.
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

// GLSL's atomicAdd returns the old value, like MSL's. HLSL takes two lines
// rather than one: InterlockedAdd hands the old value back through an out
// parameter, so the name has to exist before the call that fills it.
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

// The synchronisation point itself. The names it invalidates - anything
// computed from shared memory - are given up by the dropStale that follows
// every statement, the same pass an assignment retires its variable's readers
// through. GLSL says it in two calls: the memory barrier publishes what was
// written, the execution barrier is where the group meets.
std::string StageEmitter::emitBarrier(const Statement&, StatementScope& scope)
{
    return formatBarrierStatement(printer.backend, scope.indent);
}

// Several statements on every backend, laid down where the reduction was
// written so the barriers inside it keep their place among the stores around
// them.
std::string StageEmitter::emitGroupReduce(const Statement& statement,
                                          StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.value}, scope);
    return source + formatGroupReduction(statement, scope);
}

// The SIMD-group matrix statements, each one intrinsic on Metal and a lane's
// two elements of the fragment everywhere else, the product staging its
// operands through the threadgroup scratch.
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

// GLSL's imageStore takes a *signed* coordinate; MSL takes the colour first,
// HLSL subscripts the texture like an array.
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

// How often the statements of one block reach each node, its nested bodies
// counted in: a name is handed out only where the statement being emitted
// evaluates the node anyway, so a body's use of one costs that body nothing.
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

// A wide store's operands, named before the store rather than printed into it.
//
// The index is the one the backends disagree about. Metal stores the whole
// vector through one pointer and prints the address once; HLSL and GLSL print
// it into every subscript, so an index inlined there is evaluated N times - and
// if it reads the buffer being written, every evaluation after the first reads
// back what this very store has already put there.
// write4(out, toUInt(out[i]), v) is exactly that shape, and inlined it would
// mean one thing on Metal and another on the other two.
//
// The value is named on every backend all the same, for the reason
// holdTheRecord names a record's: a wide store is one write of one record, and
// the record is worth a name wherever it is an operation rather than a leaf.
std::string StageEmitter::holdTheVector(const Statement& statement,
                                        const std::string& indent,
                                        Vector<int>& open)
{
    auto source = std::string {};

    if (printer.backend != Backend::Metal)
        source += holdOperand(statement.index, statement.slot, indent, open);

    return source + holdOperand(statement.value, statement.slot, indent, open);
}

// One of them, unless naming it would buy nothing: something already named is
// already computed, and a leaf costs nothing however often it is repeated -
// unless it is a read of the buffer being written, which is not a matter of
// cost.
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

// A statement that leaves no variable and no buffer holding something else and
// moves no shared memory cannot have staled a name, and most do not: a break, a
// continue, and an if whose bodies only compute. Asking each open name about an
// empty set is the same walk for a guaranteed no.
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
