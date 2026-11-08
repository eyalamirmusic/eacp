#include "ShaderStageEmitter.h"

#include "ShaderDeclarations.h"
#include "ShaderGroupSpelling.h"
#include "ShaderStructEmission.h"

#include <eacp/Core/Utils/Format.h>

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <limits>

namespace eacp::GPU::ShaderEmission
{
namespace
{
constexpr auto nestedIndent = std::string_view {"    "};

std::string printBufferElement(int slot, const std::string& index)
{
    return Strings::format("buffer{}[{}]", slot, index);
}

// Whether HLSL stores a struct field of this type with one Store of its words,
// printing the value once: not a struct, a matrix or an f16 vector, which are
// stored a column or a half at a time.
bool isStoredInOneWrite(ValueType type)
{
    if (type == ValueType::Struct || isMatrix(type))
        return false;

    return !isHalf(type) || componentCount(type) == 1;
}

struct AtomicSpelling
{
    AtomicOperation operation;
    std::string_view metal;
    std::string_view hlsl;
    std::string_view glsl;
};

constexpr auto atomicSpellings = Array<AtomicSpelling, 9> {
    {AtomicOperation::Add,
     "atomic_fetch_add_explicit",
     "InterlockedAdd",
     "atomicAdd"},
    {AtomicOperation::Sub,
     "atomic_fetch_sub_explicit",
     "InterlockedAdd",
     "atomicAdd"},
    {AtomicOperation::Max,
     "atomic_fetch_max_explicit",
     "InterlockedMax",
     "atomicMax"},
    {AtomicOperation::Min,
     "atomic_fetch_min_explicit",
     "InterlockedMin",
     "atomicMin"},
    {AtomicOperation::And,
     "atomic_fetch_and_explicit",
     "InterlockedAnd",
     "atomicAnd"},
    {AtomicOperation::Or, "atomic_fetch_or_explicit", "InterlockedOr", "atomicOr"},
    {AtomicOperation::Xor,
     "atomic_fetch_xor_explicit",
     "InterlockedXor",
     "atomicXor"},
    {AtomicOperation::Exchange,
     "atomic_exchange_explicit",
     "InterlockedExchange",
     "atomicExchange"},
    {AtomicOperation::CompareExchange,
     "atomic_compare_exchange_weak_explicit",
     "InterlockedCompareExchange",
     "atomicCompSwap"},
};

std::string_view getAtomicFunction(AtomicOperation operation, Backend backend)
{
    for (const auto& spelling: atomicSpellings)
    {
        if (spelling.operation != operation)
            continue;

        if (backend == Backend::Metal)
            return spelling.metal;

        return backend == Backend::Vulkan ? spelling.glsl : spelling.hlsl;
    }

    return {};
}

// The values a switch's jump flag takes: the loop jump a clause asked for.
constexpr auto breakCode = 1;
constexpr auto continueCode = 2;

// Which loop jumps reach out of a switch's clauses, as breakCode and
// continueCode bits: through ifs and nested switches, never past a loop of
// their own.
int findLoopJumps(const ShaderGraph& graph, int block)
{
    auto jumps = 0;

    for (auto index: graph.block(block).statements)
    {
        const auto& statement = graph.statement(index);

        if (statement.kind == StatementKind::Break)
            jumps |= breakCode;

        if (statement.kind == StatementKind::Continue)
            jumps |= continueCode;

        if (statement.kind == StatementKind::Loop)
            continue;

        for (auto body: {statement.body, statement.elseBody})
            if (body >= 0)
                jumps |= findLoopJumps(graph, body);
    }

    return jumps;
}

bool endsInJump(const ShaderGraph& graph, int block)
{
    const auto& statements = graph.block(block).statements;

    if (statements.empty())
        return false;

    auto last = graph.statement(statements.back()).kind;
    return last == StatementKind::Return || last == StatementKind::Break
           || last == StatementKind::Continue;
}

// -2147483648 is the negation of a literal no int holds, which GLSL refuses.
std::string printCaseLabel(ValueType selector, std::int64_t value)
{
    if (selector == ValueType::UInt)
        return Strings::format("{}u", value);

    if (value == std::numeric_limits<std::int32_t>::min())
        return "(-2147483647 - 1)";

    return Strings::format("{}", value);
}
} // namespace

StageEmitter::StageEmitter(const ShaderGraph& graphToUse,
                           Backend backend,
                           Vector<int> attributeVaryings)
    : printer {graphToUse, backend, locals, std::move(attributeVaryings)}
    , visited(graphToUse.nodeCount())
{
    locals.resize(graphToUse.nodeCount(), -1);
    variablesInScope.resize(graphToUse.variables().size(), 0);
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

    for (auto index: getGraph().block(block).statements)
    {
        const auto& statement = getGraph().statement(index);
        auto declares =
            statement.kind == StatementKind::Declare
            || (statement.kind == StatementKind::Call && statement.slot >= 0);

        if (declares)
            variablesInScope[statement.slot] = 0;
    }

    retireLocals(open);
    return source;
}

StageEmitter::StatementEmitFunction
    StageEmitter::findStatementEmitter(StatementKind kind)
{
    static constexpr auto emitters = Array<StatementEmitEntry, 21> {
        {StatementKind::Declare, &StageEmitter::emitVariableWrite},
        {StatementKind::Assign, &StageEmitter::emitVariableWrite},
        {StatementKind::If, &StageEmitter::emitIf},
        {StatementKind::Break, &StageEmitter::emitBreak},
        {StatementKind::Continue, &StageEmitter::emitContinue},
        {StatementKind::BreakIf, &StageEmitter::emitBreakIf},
        {StatementKind::Switch, &StageEmitter::emitSwitch},
        {StatementKind::Return, &StageEmitter::emitReturn},
        {StatementKind::Discard, &StageEmitter::emitDiscard},
        {StatementKind::Store, &StageEmitter::emitStore},
        {StatementKind::VectorStore, &StageEmitter::emitVectorStore},
        {StatementKind::Atomic, &StageEmitter::emitAtomic},
        {StatementKind::SharedStore, &StageEmitter::emitSharedStore},
        {StatementKind::Barrier, &StageEmitter::emitBarrier},
        {StatementKind::GroupReduce, &StageEmitter::emitGroupReduce},
        {StatementKind::SimdMatrixFill, &StageEmitter::emitSimdMatrixFill},
        {StatementKind::SimdMatrixLoad, &StageEmitter::emitSimdMatrixTransfer},
        {StatementKind::SimdMatrixStore, &StageEmitter::emitSimdMatrixTransfer},
        {StatementKind::SimdMatrixMultiplyAdd,
         &StageEmitter::emitSimdMatrixMultiplyAdd},
        {StatementKind::TextureStore, &StageEmitter::emitTextureStore},
        {StatementKind::Call, &StageEmitter::emitCall},
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

    if (statement.continuing >= 0)
        return emitLoopWithContinuing(statement, indent);

    auto header = Strings::format(
        "{}while ({})\n", indent, printer.printReference(statement.value));
    return header + printLoopBlock(statement.body, indent);
}

std::string StageEmitter::printLoopBlock(int block, const std::string& indent)
{
    jumpTargets.add(jumpsToLoop);
    auto source = printBody(block, indent);
    jumpTargets.pop_back();
    return source;
}

// The continuing block runs at the top of every pass but the first, so a
// continue - which jumps to the top - runs it as well, which a block printed
// after the body would not.
std::string StageEmitter::emitLoopWithContinuing(const Statement& statement,
                                                 const std::string& indent)
{
    auto pass = controlCount++;
    auto inner = Strings::format("{}{}", indent, nestedIndent);

    auto source =
        Strings::format("{}bool firstPass{} = true;\n{}while (true)\n{}{{\n",
                        indent,
                        pass,
                        indent,
                        indent);
    source += Strings::format("{}if (!firstPass{})\n", inner, pass);
    source += printLoopBlock(statement.continuing, inner);
    source += Strings::format("{}firstPass{} = false;\n", inner, pass);
    source += Strings::format("{}if (!{})\n{}{{\n{}{}break;\n{}}}\n",
                              inner,
                              printer.printReference(statement.value),
                              inner,
                              inner,
                              nestedIndent,
                              inner);

    jumpTargets.add(jumpsToLoop);
    source += emitBlock(statement.body, inner);
    jumpTargets.pop_back();

    return source + indent + "}\n";
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

// A private array is declared bare and each element set to the fill in a loop,
// which every dialect spells alike; an element write names the element.
std::string StageEmitter::emitArrayWrite(const Statement& statement,
                                         StatementScope& scope)
{
    auto slot = statement.slot;

    if (statement.kind == StatementKind::Assign)
    {
        auto source = defineLocalsInScope({statement.index, statement.value}, scope);
        auto element = Strings::format(
            "v{}[{}]", slot, printer.printReference(statement.index));
        source +=
            Strings::format("{}{} = {};\n",
                            scope.indent,
                            printFieldTarget(getGraph(),
                                             getGraph().variableStructType(slot),
                                             statement.member,
                                             element),
                            printer.printReference(statement.value));
        return source;
    }

    variablesInScope[slot] = 1;

    auto size = getGraph().variableArraySize(slot);
    auto type = printer.printVariableTypeName(slot);
    auto source = defineLocalsInScope({statement.value}, scope);
    source += Strings::format("{}{} v{}[{}];\n", scope.indent, type, slot, size);
    source += Strings::format("{}for (int fill{} = 0; fill{} < {}; ++fill{})\n",
                              scope.indent,
                              slot,
                              slot,
                              size,
                              slot);
    source += Strings::format("{}{}v{}[fill{}] = {};\n",
                              scope.indent,
                              nestedIndent,
                              slot,
                              slot,
                              printer.printReference(statement.value));
    return source;
}

std::string StageEmitter::emitVariableWrite(const Statement& statement,
                                            StatementScope& scope)
{
    if (getGraph().variableArraySize(statement.slot) > 0)
        return emitArrayWrite(statement, scope);

    auto type = std::string {};

    if (statement.kind == StatementKind::Declare)
    {
        variablesInScope[statement.slot] = 1;
        type = printer.printVariableTypeName(statement.slot) + " ";
    }

    auto source = defineLocalsInScope({statement.value}, scope);
    source += Strings::format(
        "{}{}{} = {};\n",
        scope.indent,
        type,
        printFieldTarget(getGraph(),
                         getGraph().variableStructType(statement.slot),
                         statement.member,
                         Strings::format("v{}", statement.slot)),
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
    return printLoopJump(breakCode, scope.indent);
}

std::string StageEmitter::emitContinue(const Statement&, StatementScope& scope)
{
    return printLoopJump(continueCode, scope.indent);
}

// Inside a switch, a C-family break ends the clause and HLSL refuses a
// continue outright, so the jump is left in the switch's flag and taken once
// the switch is behind it.
std::string StageEmitter::printLoopJump(int code, const std::string& indent) const
{
    auto target = jumpTargets.empty() ? jumpsToLoop : jumpTargets.back();

    if (target < 0)
        return indent + (code == breakCode ? "break;\n" : "continue;\n");

    return Strings::format(
        "{}jump{} = {};\n{}break;\n", indent, target, code, indent);
}

std::string StageEmitter::emitBreakIf(const Statement& statement,
                                      StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.value}, scope);
    source += Strings::format("{}if ({})\n{}{{\n",
                              scope.indent,
                              printer.printReference(statement.value),
                              scope.indent);
    source += printLoopJump(breakCode, scope.indent + std::string(nestedIndent));
    return source + scope.indent + "}\n";
}

std::string StageEmitter::emitSwitch(const Statement& statement,
                                     StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.value}, scope);
    auto selector = printer.printReference(statement.value);
    dropStaleLocals(statement, scope.open);

    auto jumps = findLoopJumps(getGraph(), statement.body);
    auto flag = noJumpFlag;

    if (jumps != 0)
    {
        flag = controlCount++;
        source += Strings::format("{}int jump{} = 0;\n", scope.indent, flag);
    }

    source += Strings::format(
        "{}switch ({})\n{}{{\n", scope.indent, selector, scope.indent);

    const auto type = getGraph().expr(statement.value).type;
    const auto clauseIndent = scope.indent + std::string(nestedIndent);
    jumpTargets.add(flag);

    for (auto index: getGraph().block(statement.body).statements)
        source += printClause(getGraph().statement(index), type, clauseIndent);

    jumpTargets.pop_back();
    source += scope.indent + "}\n";

    if (flag >= 0)
        source += printJumpsOutOfSwitch(flag, jumps, scope.indent);

    return source;
}

// Braced, so a clause's declarations are its own; ended with a break, so none
// falls into the next.
std::string StageEmitter::printClause(const Statement& clause,
                                      ValueType selector,
                                      const std::string& indent)
{
    const auto& labels = getGraph().caseLabels(clause.slot);
    auto source = std::string {};

    for (auto value: labels.values)
        source +=
            Strings::format("{}case {}:\n", indent, printCaseLabel(selector, value));

    if (labels.isDefault)
        source += indent + "default:\n";

    const auto inner = indent + std::string(nestedIndent);
    source += indent + "{\n" + emitBlock(clause.body, inner);

    if (!endsInJump(getGraph(), clause.body))
        source += inner + "break;\n";

    return source + indent + "}\n";
}

std::string StageEmitter::printJumpsOutOfSwitch(int flag,
                                                int jumps,
                                                const std::string& indent) const
{
    auto source = std::string {};
    const auto inner = indent + std::string(nestedIndent);

    for (auto code: {breakCode, continueCode})
        if ((jumps & code) != 0)
            source += Strings::format(
                          "{}if (jump{} == {})\n{}{{\n", indent, flag, code, indent)
                      + printLoopJump(code, inner) + indent + "}\n";

    return source;
}

// The outputs are evaluated where the return stands, their shared values named
// in the block the return is in.
std::string StageEmitter::emitReturn(const Statement& statement,
                                     StatementScope& scope)
{
    if (statement.value >= 0)
        return emitValueReturn(statement, scope);

    assert(returnReadsOnlyVariablesInScope()
           && "eacp: returnEarly evaluates the outputs where it stands, so a var "
              "they read is declared before it, in a block around it.");

    auto source = defineLocals(
        returnRoots, scope.indent, countUsesOver(returnRoots), scope.open);
    return source + printReturn(*this, scope.indent);
}

// A function's return of its result.
std::string StageEmitter::emitValueReturn(const Statement& statement,
                                          StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.value}, scope);
    return source
           + Strings::format("{}return {};\n",
                             scope.indent,
                             printer.printReference(statement.value));
}

// A call made where it is written: its result declared, or the call alone.
std::string StageEmitter::emitCall(const Statement& statement, StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.value}, scope);
    auto call = printer.printReference(statement.value);

    if (statement.slot < 0)
        return source + Strings::format("{}{};\n", scope.indent, call);

    variablesInScope[statement.slot] = 1;

    return source
           + Strings::format("{}{} v{} = {};\n",
                             scope.indent,
                             printer.printVariableTypeName(statement.slot),
                             statement.slot,
                             call);
}

bool StageEmitter::returnReadsOnlyVariablesInScope()
{
    visited.restart();

    for (auto root: returnRoots)
        if (!readsOnlyVariablesInScope(root))
            return false;

    return true;
}

bool StageEmitter::readsOnlyVariablesInScope(int node)
{
    if (node < 0 || !visited.visit(node))
        return true;

    const auto& expr = getGraph().expr(node);

    if (expr.kind == ExprKind::VarRead && variablesInScope[expr.index] == 0)
        return false;

    for (auto argument: expr.args)
        if (!readsOnlyVariablesInScope(argument))
            return false;

    return true;
}

std::string StageEmitter::emitDiscard(const Statement&, StatementScope& scope)
{
    return scope.indent + (isMetal() ? "discard_fragment();\n" : "discard;\n");
}

// MSL and GLSL assign a field, or the element whole, in one statement. HLSL
// stores a value word by word, so a value or an index it prints more than
// once is named first, and every store reads what the element held before.
std::string StageEmitter::emitStructStore(const Statement& statement,
                                          StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.index, statement.value}, scope);
    auto structId = getGraph().storageStructType(statement.slot);

    if (printer.backend == Backend::DirectX)
    {
        source +=
            holdOperand(statement.index, statement.slot, scope.indent, scope.open);

        if (!isStoredInOneWrite(getGraph().expr(statement.value).type))
            source += holdOperand(
                statement.value, statement.slot, scope.indent, scope.open);

        return source
               + printHlslStructStore(getGraph(),
                                      statement.slot,
                                      printer.printReference(statement.index),
                                      statement.member,
                                      printer.printReference(statement.value),
                                      scope.indent);
    }

    auto element =
        printBufferElement(statement.slot, printer.printReference(statement.index));

    return source
           + Strings::format(
               "{}{} = {};\n",
               scope.indent,
               printFieldTarget(getGraph(), structId, statement.member, element),
               printer.printReference(statement.value));
}

std::string StageEmitter::emitStore(const Statement& statement,
                                    StatementScope& scope)
{
    if (getGraph().storageStructType(statement.slot) >= 0)
        return emitStructStore(statement, scope);

    auto source = defineLocalsInScope({statement.index, statement.value}, scope);
    source += holdTheRecord(statement, scope.indent, scope.open);

    auto element =
        printBufferElement(statement.slot, printer.printReference(statement.index));
    auto stored = printer.printReference(statement.value);
    auto atomic =
        getGraph().storageBuffers()[statement.slot] == BufferAccess::Atomic;
    auto halfWords =
        printer.backend == Backend::DirectX
        && getGraph().storageElementType(statement.slot) == ValueType::Half;

    if (halfWords)
        source += printHlslHalfStore(
            statement.slot, printer.printReference(statement.index), stored, scope);
    else if (atomic && printer.backend == Backend::Metal)
        source += Strings::format(
            "{}atomic_store_explicit(&{}, {}, memory_order_relaxed);\n",
            scope.indent,
            element,
            stored);
    else
        source += Strings::format("{}{} = {};\n", scope.indent, element, stored);

    return source;
}

// One half of an HLSL f16 buffer's word: clear this element's sixteen bits,
// then set them, each an atomic operation on the word, so a thread writing the
// word's other half at the same time loses nothing.
std::string StageEmitter::printHlslHalfStore(int slot,
                                             const std::string& index,
                                             const std::string& value,
                                             const StatementScope& scope) const
{
    auto word = Strings::format("buffer{}[({}) >> 1u]", slot, index);
    auto shift = Strings::format("((({}) & 1u) << 4u)", index);

    return Strings::format("{0}InterlockedAnd({1}, ~(0xffffu << {2}));\n"
                           "{0}InterlockedOr({1}, f32tof16({3}) << {2});\n",
                           scope.indent,
                           word,
                           shift,
                           value);
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

// HLSL's Interlocked* return the old value through an out parameter, and
// neither HLSL nor GLSL has a subtraction: Sub adds the negated operand.
std::string StageEmitter::emitAtomic(const Statement& statement,
                                     StatementScope& scope)
{
    auto source = defineLocalsInScope(
        {statement.index, statement.value, statement.compare}, scope);

    auto element = statement.memory == MemorySpace::Buffer
                       ? printBufferElement(statement.bufferSlot,
                                            printer.printReference(statement.index))
                       : Strings::format("s{}[{}]",
                                         statement.bufferSlot,
                                         printer.printReference(statement.index));

    auto operand = printer.printReference(statement.value);
    auto type = getGraph().variables()[statement.slot];
    auto call = AtomicCall {element,
                            operand,
                            std::string(getDialectTypeName(printer.backend, type)),
                            Strings::format("v{}", statement.slot),
                            scope.indent};

    if (statement.atomic == AtomicOperation::CompareExchange)
        return source + formatCompareExchange(statement, call);

    if (statement.atomic == AtomicOperation::Sub && !isMetal())
        call.operand = type == ValueType::Int
                           ? Strings::format("-({})", operand)
                           : Strings::format("0u - ({})", operand);

    auto function = getAtomicFunction(statement.atomic, printer.backend);

    if (isMetal())
        return source
               + Strings::format("{}{} {} = {}(&{}, {}, memory_order_relaxed);\n",
                                 call.indent,
                                 call.type,
                                 call.name,
                                 function,
                                 call.element,
                                 call.operand);

    if (printer.backend == Backend::Vulkan)
        return source
               + Strings::format("{}{} {} = {}({}, {});\n",
                                 call.indent,
                                 call.type,
                                 call.name,
                                 function,
                                 call.element,
                                 call.operand);

    source += Strings::format("{}{} {};\n", call.indent, call.type, call.name);
    source += Strings::format("{}{}({}, {}, {});\n",
                              call.indent,
                              function,
                              call.element,
                              call.operand,
                              call.name);
    return source;
}

// Metal's compare-exchange is weak and writes the element's value into the
// comparand's place when it fails, so the old value starts as the comparand and
// the call says whether it stored. HLSL and GLSL are strong and say nothing, so
// the comparand is named first and compared with what came back.
std::string StageEmitter::formatCompareExchange(const Statement& statement,
                                                const AtomicCall& call)
{
    auto comparand = printer.printReference(statement.compare);
    auto exchanged = Strings::format("v{}", statement.exchanged);
    auto function =
        getAtomicFunction(AtomicOperation::CompareExchange, printer.backend);

    if (isMetal())
        return Strings::format(
                   "{}{} {} = {};\n", call.indent, call.type, call.name, comparand)
               + Strings::format(
                   "{}bool {} = {}(&{}, &{}, {}, memory_order_relaxed, "
                   "memory_order_relaxed);\n",
                   call.indent,
                   exchanged,
                   function,
                   call.element,
                   call.name,
                   call.operand);

    auto held = Strings::format("cx{}", statement.slot);
    auto source =
        Strings::format("{}{} {} = {};\n", call.indent, call.type, held, comparand);

    if (printer.backend == Backend::Vulkan)
        source += Strings::format("{}{} {} = {}({}, {}, {});\n",
                                  call.indent,
                                  call.type,
                                  call.name,
                                  function,
                                  call.element,
                                  held,
                                  call.operand);
    else
        source += Strings::format("{}{} {};\n", call.indent, call.type, call.name)
                  + Strings::format("{}{}({}, {}, {}, {});\n",
                                    call.indent,
                                    function,
                                    call.element,
                                    held,
                                    call.operand,
                                    call.name);

    return source
           + Strings::format(
               "{}bool {} = {} == {};\n", call.indent, exchanged, call.name, held);
}

std::string StageEmitter::emitSharedStore(const Statement& statement,
                                          StatementScope& scope)
{
    auto source = defineLocalsInScope({statement.index, statement.value}, scope);
    auto element = Strings::format(
        "s{}[{}]", statement.slot, printer.printReference(statement.index));
    auto stored = printer.printReference(statement.value);

    if (isMetal() && getGraph().sharedArrays()[statement.slot].atomic)
        return source
               + Strings::format(
                   "{}atomic_store_explicit(&{}, {}, memory_order_relaxed);\n",
                   scope.indent,
                   element,
                   stored);

    return source + Strings::format("{}{} = {};\n", scope.indent, element, stored);
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
    if (statement.indexY < 0)
        return emitStorageStore(statement, scope);

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

// A typed UAV takes as many components as its format has. A Metal read_write
// texture orders a later read of what this thread wrote only past a fence;
// HLSL and GLSL keep one invocation's image accesses in program order.
std::string StageEmitter::emitStorageStore(const Statement& statement,
                                           StatementScope& scope)
{
    const auto& indent = scope.indent;
    auto source = defineLocalsInScope(
        {statement.index, statement.layer, statement.value}, scope);

    const auto& graph = getGraph();
    const auto address = printer.printStorageAddress(
        statement.slot, statement.index, statement.layer);
    const auto value = printer.printReference(statement.value);

    if (printer.backend == Backend::Metal)
    {
        source += Strings::format(
            "{}texture{}.write({}, {});\n", indent, statement.slot, value, address);

        if (graph.textureAccess(statement.slot) == TextureAccess::ReadWrite)
            source +=
                Strings::format("{}texture{}.fence();\n", indent, statement.slot);

        return source;
    }

    if (printer.backend == Backend::Vulkan)
        return source
               + Strings::format("{}imageStore(texture{}, {}, {});\n",
                                 indent,
                                 statement.slot,
                                 address,
                                 value);

    const auto channels =
        getStorageChannelCount(graph.textureFormat(statement.slot));
    const auto stored = channels >= 4
                            ? value
                            : Strings::format("{}.{}",
                                              wrapInParentheses(value),
                                              std::string_view {"xyzw"}.substr(
                                                  0, (std::size_t) channels));

    return source
           + Strings::format(
               "{}texture{}[{}] = {};\n", indent, statement.slot, address, stored);
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

    return Strings::format("{}{} t{} = {};\n",
                           indent,
                           printer.printTypeName(node),
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
