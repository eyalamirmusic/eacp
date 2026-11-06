#include "ShaderGroupSpelling.h"
#include "ShaderStageEmitter.h"

#include <eacp/Core/Utils/Format.h>

namespace eacp::GPU::ShaderEmission
{

// MSL has the SIMD-group intrinsics, so a fold scoped to one is a single
// instruction and a fold over a wider group is those partials combined through
// the scratch. HLSL under FXC has no wave intrinsic at cs_5_0, and GLSL's
// subgroup extension is both unassumable and the wrong width - a subgroup is
// whatever the device says it is, where eacp's simdWidth is 32 everywhere by
// construction - so both take the scratch tree, narrowed to the folding
// thread's own block of lanes where the scope is the SIMD group. Either way the
// result lands in the reduction's variable on every thread, and a trailing
// barrier leaves the scratch free for the next one.
std::string StageEmitter::formatGroupReduction(const Statement& statement,
                                               const StatementScope& scope)
{
    if (printer.backend == Backend::Metal)
        return formatMetalGroupReduction(statement, scope);

    return formatTreeGroupReduction(statement, scope);
}

std::string StageEmitter::formatMetalGroupReduction(const Statement& statement,
                                                    const StatementScope& scope)
{
    const auto& indent = scope.indent;

    auto elementType = getGraph().variables()[statement.slot];
    auto name = Strings::format("v{}", statement.slot);
    auto scratch = std::string(getGroupScratchName(elementType));
    auto step = Strings::format("gr{}", statement.slot);
    auto barrier = formatBarrierStatement(printer.backend, indent);

    auto source = Strings::format("{}{} {} = {}({});\n",
                                  indent,
                                  getDialectTypeName(printer.backend, elementType),
                                  name,
                                  getMetalSimdReduction(statement.reduction),
                                  printer.printReference(statement.value));

    if (doesMetalFoldInOneInstruction(statement.scope))
        return source;

    source += Strings::format("{}if (simdLane == 0u)\n{}    {}[simdIndex] = {};\n",
                              indent,
                              indent,
                              scratch,
                              name);
    source += barrier;
    source += Strings::format("{}{} = {}[0];\n", indent, name, scratch);
    source += Strings::format(
        "{}for (uint {} = 1u; {} < simdCount; ++{})\n", indent, step, step, step);
    source += Strings::format(
        "{}    {} = {};\n",
        indent,
        name,
        formatFoldedPair(statement.reduction, name, scratch + "[" + step + "]"));

    return source + barrier;
}

// Within the fold, a lane counts from the start of its own block; the answer is
// read from that block's first slot. Both collapse to the plain lane and slot
// zero where the block is the whole group, which is what keeps the wide fold
// spelled exactly as it always was.
//
// The bound on the partner is the scratch's own, and it is not implied by the
// fold's width: a narrow fold indexes with the global lane while it counts with
// the lane within its block, so a group that is not a whole number of SIMD
// groups - which the assert in emitCompute refuses, and which a release build
// does not - would otherwise read past the array.
std::string StageEmitter::formatTreeGroupReduction(const Statement& statement,
                                                   const StatementScope& scope)
{
    const auto& indent = scope.indent;

    auto elementType = getGraph().variables()[statement.slot];
    auto scratch = std::string(getGroupScratchName(elementType));
    auto step = Strings::format("gr{}", statement.slot);
    auto barrier = formatBarrierStatement(printer.backend, indent);

    auto lane = std::string(getGroupLaneName(printer.backend));
    auto width = getReductionWidth(getGraph(), statement.scope);
    auto whole = doesFoldSpanWholeGroup(getGraph(), statement.scope);
    auto widthText = Strings::format("{}u", width);

    auto within = whole ? lane : wrapInParentheses(lane + " % " + widthText);
    auto first =
        whole ? std::string("0")
              : wrapInParentheses(lane + " / " + widthText) + " * " + widthText;
    auto bounded =
        whole ? std::string {}
              : Strings::format(
                    " && {} + {} < {}u", lane, step, getThreadsPerGroup(getGraph()));

    auto element = Strings::format("{}[{}]", scratch, lane);
    auto partner = Strings::format("{}[{} + {}]", scratch, lane, step);

    auto source = Strings::format(
        "{}{} = {};\n", indent, element, printer.printReference(statement.value));
    source += barrier;
    source += Strings::format("{}for (uint {} = {}u; {} > 0u; {} >>= 1u)\n{}{{\n",
                              indent,
                              step,
                              getReductionStride(width),
                              step,
                              step,
                              indent);
    source += Strings::format("{}    if ({} < {} && {} + {} < {}{})\n",
                              indent,
                              within,
                              step,
                              within,
                              step,
                              widthText,
                              bounded);
    source +=
        Strings::format("{}        {} = {};\n",
                        indent,
                        element,
                        formatFoldedPair(statement.reduction, element, partner));
    source += formatBarrierStatement(printer.backend, indent + "    ");
    source += indent + "}\n";
    source += Strings::format("{}{} v{} = {}[{}];\n",
                              indent,
                              getDialectTypeName(printer.backend, elementType),
                              statement.slot,
                              scratch,
                              first);

    return source + barrier;
}

bool StageEmitter::isMetal() const
{
    return printer.backend == Backend::Metal;
}

// The four matrix statements below. An 8x8 fragment on Metal is a type MSL has,
// and each operation on one is a single intrinsic. On the two backends with no
// wave matrix operation it is spread over the lanes the way Metal spreads it -
// see simdMatrixLaneElements - so a fill, a load and a store are two scalars of
// each lane's own, and only the product needs what other lanes hold, which it
// fetches through the threadgroup scratch between two barriers: the exchange a
// wave intrinsic does in registers.
//
// Correctness at whatever it costs, as the reduction's tree is where there is
// no wave intrinsic - and unlike that tree, this is not the fastest form the
// hardware would allow, which is why the README says so.
//
// The fallback holds every fragment as a lane's pair of floats, whatever the
// memory it came out of: what a packed load changes there is the arithmetic
// that produces the pair, not the fragment.
std::string StageEmitter::formatSimdMatrixDeclaration(int slot) const
{
    if (isMetal())
        return Strings::format(
            "{} {}",
            getMetalSimdMatrixType(getGraph().simdMatrixElement(slot)),
            getSimdMatrixName(slot));

    return Strings::format(
        "{} {}", getSimdMatrixLaneType(printer.backend), getSimdMatrixName(slot));
}

std::string StageEmitter::formatSimdMatrixFill(const Statement& statement,
                                               const StatementScope& scope)
{
    const auto& indent = scope.indent;

    auto value = printer.printReference(statement.value);
    auto declaration = formatSimdMatrixDeclaration(statement.slot);

    if (isMetal())
        return Strings::format(
            "{}{} = make_filled_simdgroup_matrix<float, {}, {}>({});\n",
            indent,
            declaration,
            simdMatrixSize,
            simdMatrixSize,
            value);

    return Strings::format("{}{} = {}({}, {});\n",
                           indent,
                           declaration,
                           getSimdMatrixLaneType(printer.backend),
                           value,
                           value);
}

// One of the two elements of a patch a lane holds, as the memory names it: the
// lane's row at the first of its pair of columns or the one after.
//
// Where the patch is packed the index counts sixteen-bit elements, so the word
// holding one is at half that index and which half of it is the parity -
// exactly the arithmetic InputBuffer::readHalf and readBFloat16 do, through the
// same helper. Each element is fetched on its own rather than a lane's pair
// taken out of one word. The pair is two adjacent columns, but the patch's
// offset and row stride are the caller's and neither has to be even, so the
// pair is not reliably inside one word and a walk that assumed it was would
// read the wrong element on every odd row.
std::string StageEmitter::formatSimdMatrixLaneElement(const Statement& statement,
                                                      const SimdMatrixPatch& patch,
                                                      int which) const
{
    auto element = getGraph().simdMatrixElement(statement.slot);
    auto position = Strings::format("{} + sgmRow * {} + {}",
                                    patch.offset,
                                    patch.stride,
                                    printSimdMatrixLaneColumn(which));

    if (element == SimdMatrixElement::Float)
        return Strings::format("{}[{}]", patch.memory, position);

    auto index = wrapInParentheses(position);

    return Strings::format("{}({}({}[{} / 2u]), {} % 2u)",
                           getPackedSimdMatrixHelper(element),
                           getBitsOfFloatFunction(printer.backend),
                           patch.memory,
                           index,
                           index);
}

// The load and the store are the same patch walked in the two directions, so
// they are one function: what changes is which side of the assignment each is
// on. Every lane moves the pair it holds, so the store needs no guard to make
// it once - no lane's share is another's.
std::string StageEmitter::formatSimdMatrixTransfer(const Statement& statement,
                                                   const StatementScope& scope)
{
    const auto& indent = scope.indent;

    auto patch = SimdMatrixPatch {
        getSimdMatrixMemoryName(statement),
        wrapInParentheses(printer.printReference(statement.index)),
        wrapInParentheses(printer.printReference(statement.stride))};

    if (isMetal())
        return formatMetalSimdMatrixTransfer(statement, patch, scope);

    auto first = formatSimdMatrixLaneElement(statement, patch, 0);
    auto second = formatSimdMatrixLaneElement(statement, patch, 1);

    if (statement.kind == StatementKind::SimdMatrixLoad)
        return Strings::format("{}{} = {}({}, {});\n",
                               indent,
                               formatSimdMatrixDeclaration(statement.slot),
                               getSimdMatrixLaneType(printer.backend),
                               first,
                               second);

    auto name = getSimdMatrixName(statement.slot);

    return Strings::format(
        "{}{} = {}.x;\n{}{} = {}.y;\n", indent, first, name, indent, second, name);
}

// The buffer is declared float whatever it holds, so a packed load reinterprets
// its pointer the way the wide packed reads reinterpret one - and then counts
// in the packed element, which is what makes the offset the caller's own row
// arithmetic.
std::string StageEmitter::formatMetalSimdMatrixTransfer(const Statement& statement,
                                                        const SimdMatrixPatch& patch,
                                                        const StatementScope& scope)
{
    const auto& indent = scope.indent;

    auto name = getSimdMatrixName(statement.slot);
    auto element = getGraph().simdMatrixElement(statement.slot);

    auto pointer = element != SimdMatrixElement::Float
                       ? Strings::format("(device const {}*) ({}) + {}",
                                         getMetalPackedElementType(element),
                                         patch.memory,
                                         patch.offset)
                       : Strings::format("{} + {}", patch.memory, patch.offset);

    if (statement.kind == StatementKind::SimdMatrixLoad)
        return Strings::format("{}{};\n{}simdgroup_load({}, {}, {});\n",
                               indent,
                               formatSimdMatrixDeclaration(statement.slot),
                               indent,
                               name,
                               pointer,
                               patch.stride);

    return Strings::format(
        "{}simdgroup_store({}, {}, {});\n", indent, name, pointer, patch.stride);
}

// Both operands staged whole, each lane putting down the pair it holds; then,
// once every pair is there, each lane takes its row of the left against its two
// columns of the right. The accumulator is allowed to be an operand, and the
// staging is what makes that safe: what is multiplied is the copy in the
// scratch, complete before anything is added. The trailing barrier is what lets
// the next product stage over this one.
std::string StageEmitter::formatSimdMatrixMultiplyAdd(const Statement& statement,
                                                      const StatementScope& scope)
{
    const auto& indent = scope.indent;

    auto accumulator = getSimdMatrixName(statement.slot);

    if (isMetal())
        return Strings::format("{}simdgroup_multiply_accumulate({}, {}, {}, {});\n",
                               indent,
                               accumulator,
                               getSimdMatrixName(statement.left),
                               getSimdMatrixName(statement.right),
                               accumulator);

    auto step = accumulator + "k";
    auto term = accumulator + "l";
    auto side = std::to_string(simdMatrixSize);
    auto barrier = formatBarrierStatement(printer.backend, indent);

    auto source = stageSimdMatrixOperands(statement, scope);
    source += barrier;
    source += Strings::format(
        "{}for (uint {} = 0u; {} < {}u; ++{})\n", indent, step, step, side, step);
    source += indent + "{\n";
    source += Strings::format(
        "{}    float {} = {};\n",
        indent,
        term,
        printStagedSimdMatrixElement(
            StagedOperand::Left, Strings::format("sgmRow * {}u + {}", side, step)));
    source +=
        Strings::format("{}    {}.x += {} * {};\n",
                        indent,
                        accumulator,
                        term,
                        printStagedSimdMatrixElement(
                            StagedOperand::Right,
                            Strings::format("{} * {}u + sgmColumn", step, side)));
    source += Strings::format(
        "{}    {}.y += {} * {};\n",
        indent,
        accumulator,
        term,
        printStagedSimdMatrixElement(
            StagedOperand::Right,
            Strings::format("{} * {}u + sgmColumn + 1u", step, side)));
    source += indent + "}\n";
    return source + barrier;
}

std::string StageEmitter::stageSimdMatrixOperands(const Statement& statement,
                                                  const StatementScope& scope)
{
    const auto& indent = scope.indent;

    auto left = getSimdMatrixName(statement.left);
    auto right = getSimdMatrixName(statement.right);
    auto held = Strings::format("sgmRow * {}u + sgmColumn", simdMatrixSize);
    auto heldNext = held + " + 1u";

    auto source = std::string {};

    for (const auto& [operand, name]: {std::pair {StagedOperand::Left, left},
                                       std::pair {StagedOperand::Right, right}})
    {
        source += Strings::format("{}{} = {}.x;\n",
                                  indent,
                                  printStagedSimdMatrixElement(operand, held),
                                  name);
        source += Strings::format("{}{} = {}.y;\n",
                                  indent,
                                  printStagedSimdMatrixElement(operand, heldNext),
                                  name);
    }

    return source;
}
} // namespace eacp::GPU::ShaderEmission
