#include "ShaderGroupSpelling.h"
#include "ShaderStageEmitter.h"

#include <eacp/Core/Utils/Format.h>

namespace eacp::GPU::ShaderEmission
{
namespace
{
constexpr auto simdMatrixScratchName = std::string_view {"sgmScratch"};

// The largest power of two below the count, so a ragged tail folds in first.
int getReductionStride(int threads)
{
    auto stride = 1;

    while (stride * 2 < threads)
        stride *= 2;

    return stride;
}

// A group smaller than a SIMD group is its own SIMD group.
int getReductionWidth(const ShaderGraph& graph, ReductionScope scope)
{
    auto threads = getThreadsPerGroup(graph);

    if (scope == ReductionScope::Group)
        return threads;

    return threads < simdGroupWidth ? threads : simdGroupWidth;
}

bool doesFoldSpanWholeGroup(const ShaderGraph& graph, ReductionScope scope)
{
    return getReductionWidth(graph, scope) == getThreadsPerGroup(graph);
}

// Only the narrow scope: the hardware SIMD width is a pipeline property, and
// an Intel Mac runs eight or sixteen lanes, so a small group may be several.
bool doesMetalFoldInOneInstruction(ReductionScope scope)
{
    return scope == ReductionScope::Simd;
}

std::string getSimdMatrixMemoryName(const Statement& statement)
{
    auto prefix = statement.memory == SimdMatrixMemory::Shared ? "s" : "buffer";
    return Strings::format("{}{}", prefix, statement.bufferSlot);
}

// Each lane holds elements 2 * lane and 2 * lane + 1 of the row-major patch.
// Not the whole 8x8: FXC caps a kernel's per-thread arrays at 4096 registers.
constexpr int simdMatrixLaneElements =
    simdMatrixSize * simdMatrixSize / simdGroupWidth;
constexpr int simdMatrixLanesPerRow = simdMatrixSize / simdMatrixLaneElements;

static_assert(simdMatrixLaneElements == 2,
              "the fallback holds a lane's share of a fragment as a two-vector");

std::string_view getSimdMatrixLaneType(Backend backend)
{
    return getDialectTypeName(backend, ValueType::Float2);
}

constexpr int simdMatrixScratchPerGroup = 2 * simdMatrixSize * simdMatrixSize;

int getSimdMatrixScratchElementCount(const ShaderGraph& graph)
{
    return graph.threadGroupShape().threadCount() / simdGroupWidth
           * simdMatrixScratchPerGroup;
}

// simdgroup_multiply_accumulate takes packed operands directly, so nothing widens.
constexpr std::string_view getMetalSimdMatrixType(SimdMatrixElement element)
{
    switch (element)
    {
        case SimdMatrixElement::Half:
            return "simdgroup_half8x8";
        case SimdMatrixElement::BFloat16:
            return "simdgroup_bfloat8x8";
        case SimdMatrixElement::Float:
            break;
    }

    return "simdgroup_float8x8";
}

constexpr std::string_view getMetalPackedElementType(SimdMatrixElement element)
{
    return element == SimdMatrixElement::Half ? "half" : "bfloat";
}

constexpr std::string_view getPackedSimdMatrixHelper(SimdMatrixElement element)
{
    return element == SimdMatrixElement::Half ? "eacpReadHalf" : "eacpReadBFloat16";
}

constexpr std::string_view getBitsOfFloatFunction(Backend backend)
{
    return backend == Backend::Vulkan ? "floatBitsToUint" : "asuint";
}

} // namespace

int getThreadsPerGroup(const ShaderGraph& graph)
{
    return graph.threadGroupShape().threadCount();
}

bool doesMetalCombinePartials(const ShaderGraph& graph)
{
    return !graph.wholeGroupReductionTypes().empty();
}

std::string formatSimdMatrixScratchDeclaration(const ShaderGraph& graph,
                                               std::string_view qualifier)
{
    if (graph.simdMatrixCount() == 0)
        return {};

    return Strings::format("{} float {}[{}];\n",
                           qualifier,
                           simdMatrixScratchName,
                           getSimdMatrixScratchElementCount(graph));
}

bool hasGroupMemoryDeclarations(const ShaderGraph& graph)
{
    return graph.sharedArrays().size() > 0 || graph.usesGroupReduction()
           || graph.simdMatrixCount() > 0;
}

std::string formatSimdMatrixPreamble(Backend backend)
{
    auto lane = getGroupLaneName(backend);

    auto source =
        Strings::format("    uint sgmLane = {} % {}u;\n", lane, simdGroupWidth);
    source +=
        Strings::format("    uint sgmRow = sgmLane / {}u;\n", simdMatrixLanesPerRow);
    source += Strings::format("    uint sgmColumn = (sgmLane % {}u) * {}u;\n",
                              simdMatrixLanesPerRow,
                              simdMatrixLaneElements);
    source += Strings::format("    uint sgmBase = ({} / {}u) * {}u;\n",
                              lane,
                              simdGroupWidth,
                              simdMatrixScratchPerGroup);
    return source;
}

std::string formatFoldedPair(GroupReduction operation,
                             const std::string& left,
                             const std::string& right)
{
    switch (operation)
    {
        case GroupReduction::Sum:
            return Strings::format("{} + {}", left, right);
        case GroupReduction::Max:
            return Strings::format("max({}, {})", left, right);
        case GroupReduction::Min:
            return Strings::format("min({}, {})", left, right);
    }

    return Strings::format("{} + {}", left, right);
}

std::string formatBarrierStatement(Backend backend, const std::string& indent)
{
    if (backend == Backend::Vulkan)
        return Strings::format(
            "{}memoryBarrierShared();\n{}barrier();\n", indent, indent);

    auto barrier = backend == Backend::Metal
                       ? "threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                       : "GroupMemoryBarrierWithGroupSync();\n";
    return indent + barrier;
}

std::string getSimdMatrixName(int slot)
{
    return Strings::format("sgm{}", slot);
}

std::string printStagedSimdMatrixElement(StagedOperand operand,
                                         std::string_view element)
{
    if (operand == StagedOperand::Right)
        return Strings::format("{}[sgmBase + {}u + {}]",
                               simdMatrixScratchName,
                               simdMatrixSize * simdMatrixSize,
                               element);

    return Strings::format("{}[sgmBase + {}]", simdMatrixScratchName, element);
}

std::string printSimdMatrixLaneColumn(int which)
{
    if (which == 0)
        return "sgmColumn";

    return Strings::format("sgmColumn + {}u", which);
}
// FXC has no wave intrinsics and GLSL subgroups have no fixed width, so both
// fold through a scratch tree.
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
        formatFoldedPair(
            statement.reduction, name, Strings::format("{}[{}]", scratch, step)));

    return source + barrier;
}

// The partner bound is the scratch's own: a group that is not a whole number
// of SIMD groups, refused only by an assert, would otherwise read past it.
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

    auto within =
        whole ? lane
              : wrapInParentheses(Strings::format("{} % {}", lane, widthText));
    auto first = whole
                     ? std::string("0")
                     : Strings::format("({} / {}) * {}", lane, widthText, widthText);
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

// Without wave matrix operations a fragment is spread over the lanes, and the
// product exchanges through threadgroup scratch between two barriers.
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

// Each element is fetched alone: the caller's offset and stride need not be
// even, so a lane's pair may straddle two words.
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

// Staging both operands first is what lets the accumulator be an operand.
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

    auto step = Strings::format("{}k", accumulator);
    auto term = Strings::format("{}l", accumulator);
    auto side = simdMatrixSize;
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
    auto heldNext = Strings::format("{} + 1u", held);

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
