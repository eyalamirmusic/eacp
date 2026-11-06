#include "PlanBuilder.h"

#include <Miro/Reflection/ReflectEnum.h>

#include <algorithm>

namespace eacp::GPU::CpuCompute
{
namespace
{
struct MathName
{
    std::string_view name;
    MathFunction function;
};

constexpr auto mathNames = Array<MathName, 22> {
    {"sin", MathFunction::Sin},     {"cos", MathFunction::Cos},
    {"tan", MathFunction::Tan},     {"asin", MathFunction::Asin},
    {"acos", MathFunction::Acos},   {"atan", MathFunction::Atan},
    {"sinh", MathFunction::Sinh},   {"cosh", MathFunction::Cosh},
    {"tanh", MathFunction::Tanh},   {"exp", MathFunction::Exp},
    {"exp2", MathFunction::Exp2},   {"log", MathFunction::Log},
    {"log2", MathFunction::Log2},   {"log10", MathFunction::Log10},
    {"sqrt", MathFunction::Sqrt},   {"rsqrt", MathFunction::Rsqrt},
    {"floor", MathFunction::Floor}, {"ceil", MathFunction::Ceil},
    {"trunc", MathFunction::Trunc}, {"round", MathFunction::Round},
    {"fract", MathFunction::Fract}, {"sign", MathFunction::Sign}};

struct NamedOp
{
    std::string_view name;
    Op op;
    int arguments;
};

constexpr auto geometricNames =
    Array<NamedOp, 8> {{"dot", Op::Dot, 2},
                       {"length", Op::Length, 1},
                       {"distance", Op::Distance, 2},
                       {"normalize", Op::Normalize, 1},
                       {"cross", Op::Cross, 2},
                       {"reflect", Op::Reflect, 2},
                       {"refract", Op::Refract, 3},
                       {"faceforward", Op::FaceForward, 3}};

constexpr auto broadcastNames =
    Array<NamedOp, 6> {{"pow", Op::Pow, 2},
                       {"atan2", Op::Atan2, 2},
                       {"step", Op::Step, 2},
                       {"clamp", Op::Clamp, 3},
                       {"mix", Op::Mix, 3},
                       {"smoothstep", Op::Smoothstep, 3}};

struct HelperName
{
    std::string_view name;
    HelperFunction function;
};

// The helpers applied per component, at any float width.
constexpr auto componentHelperNames =
    Array<HelperName, 3> {{"eacpErf", HelperFunction::Erf},
                          {"eacpErfc", HelperFunction::Erfc},
                          {"eacpSaturatingTanh", HelperFunction::SaturatingTanh}};

// The second argument type is read only when the helper takes two.
struct HelperSignature
{
    ValueType result;
    int arguments;
    ValueType first;
    ValueType second;
};

constexpr auto readSignature =
    HelperSignature {ValueType::Float, 2, ValueType::UInt, ValueType::UInt};
constexpr auto unpackPairSignature =
    HelperSignature {ValueType::Float2, 1, ValueType::UInt, ValueType::UInt};
constexpr auto packPairSignature =
    HelperSignature {ValueType::UInt, 1, ValueType::Float2, ValueType::UInt};
constexpr auto unpackQuadSignature =
    HelperSignature {ValueType::Float4, 1, ValueType::UInt, ValueType::UInt};
constexpr auto packIntQuadSignature =
    HelperSignature {ValueType::UInt, 1, ValueType::Int4, ValueType::UInt};
constexpr auto packUIntQuadSignature =
    HelperSignature {ValueType::UInt, 1, ValueType::UInt4, ValueType::UInt};

struct PackedHelperName
{
    std::string_view name;
    HelperFunction function;
    HelperSignature signature;
};

// The packed-data helpers, each with the one signature the shader defines.
constexpr auto packedHelperNames = Array<PackedHelperName, 14> {
    {"eacpUnpackHalf2", HelperFunction::UnpackHalf2, unpackPairSignature},
    {"eacpPackHalf2", HelperFunction::PackHalf2, packPairSignature},
    {"eacpReadHalf", HelperFunction::ReadHalf, readSignature},
    {"eacpUnpackBFloat16x2", HelperFunction::UnpackBFloat16x2, unpackPairSignature},
    {"eacpPackBFloat16x2", HelperFunction::PackBFloat16x2, packPairSignature},
    {"eacpReadBFloat16", HelperFunction::ReadBFloat16, readSignature},
    {"eacpReadInt8", HelperFunction::ReadInt8, readSignature},
    {"eacpReadUInt8", HelperFunction::ReadUInt8, readSignature},
    {"eacpUnpackInt8x4", HelperFunction::UnpackInt8x4, unpackQuadSignature},
    {"eacpUnpackUInt8x4", HelperFunction::UnpackUInt8x4, unpackQuadSignature},
    {"eacpUnpackInt4x4", HelperFunction::UnpackInt4x4, unpackQuadSignature},
    {"eacpUnpackUInt4x4", HelperFunction::UnpackUInt4x4, unpackQuadSignature},
    {"eacpPackInt8x4", HelperFunction::PackInt8x4, packIntQuadSignature},
    {"eacpPackUInt8x4", HelperFunction::PackUInt8x4, packUIntQuadSignature}};

// Op::Leaf marks an operator the type family does not have.
constexpr auto noBinaryOp = Op::Leaf;

struct BinaryOperator
{
    char symbol;
    Op floatOp;
    Op unsignedOp;
    Op signedOp;
};

constexpr auto binaryOperators =
    Array<BinaryOperator, 8> {{'+', Op::AddF, Op::AddI, Op::AddI},
                              {'-', Op::SubF, Op::SubI, Op::SubI},
                              {'*', Op::MulF, Op::MulI, Op::MulI},
                              {'/', Op::DivF, Op::DivU, Op::DivS},
                              {'%', noBinaryOp, Op::RemU, Op::RemS},
                              {'&', noBinaryOp, Op::And, Op::And},
                              {'|', noBinaryOp, Op::Or, Op::Or},
                              {'^', noBinaryOp, Op::Xor, Op::Xor}};

struct BoolRelation
{
    std::string_view text;
    Op op;
};

constexpr auto boolRelations = Array<BoolRelation, 4> {
    {"==", Op::EqMask}, {"!=", Op::Xor}, {"&&", Op::And}, {"||", Op::Or}};

struct RelationName
{
    std::string_view text;
    Relation relation;
};

constexpr auto relationNames =
    Array<RelationName, 6> {{"<", Relation::Less},
                            {"<=", Relation::LessEqual},
                            {">", Relation::Greater},
                            {">=", Relation::GreaterEqual},
                            {"==", Relation::Equal},
                            {"!=", Relation::NotEqual}};

constexpr auto derivativeNames =
    Array<std::string_view, 3> {"dfdx", "dfdy", "fwidth"};

constexpr auto conversionTargets = Array<ValueType, 12> {ValueType::Float,
                                                         ValueType::Float2,
                                                         ValueType::Float3,
                                                         ValueType::Float4,
                                                         ValueType::UInt,
                                                         ValueType::UInt2,
                                                         ValueType::UInt3,
                                                         ValueType::UInt4,
                                                         ValueType::Int,
                                                         ValueType::Int2,
                                                         ValueType::Int3,
                                                         ValueType::Int4};

bool isConversionName(std::string_view name)
{
    return std::ranges::any_of(conversionTargets,
                               [name](ValueType type)
                               { return name == typeName(type); });
}

bool isThreadIdShape(int index, int components)
{
    if (index == allComponents)
        return components <= PlanBuilder::axisCount;

    return index >= 0 && index < PlanBuilder::axisCount;
}

bool isFloatVector(ValueType type)
{
    return isFloatFamily(type) && !isMatrix(type);
}

bool isShiftOperator(std::string_view text)
{
    return text == "<<" || text == ">>";
}

// A vector of the order's width, which an order-sized matrix multiplies.
bool isVectorFor(ValueType vector, int order)
{
    return order > 0 && matrixOrder(vector) == 0 && componentCount(vector) == order
           && isFloatFamily(vector);
}

std::string_view getMissingAxisReason(ExprKind kind)
{
    if (kind == ExprKind::LocalId)
        return "names no axis of the local id";

    if (kind == ExprKind::GroupId)
        return "names no axis of the group id";

    return "names no axis of the thread id";
}

struct UnaryOperator
{
    char symbol;
    bool (*accepts)(ValueType);
    Op op;
};

constexpr auto unaryOperators =
    Array<UnaryOperator, 4> {{'-', isFloatFamily, Op::NegF},
                             {'-', isSignedInteger, Op::NegI},
                             {'!', isBoolean, Op::BitNot},
                             {'~', isIntegerFamily, Op::BitNot}};

std::optional<Op> findUnaryOp(char symbol, ValueType type)
{
    auto found = std::ranges::find_if(
        unaryOperators,
        [&](const UnaryOperator& entry)
        { return entry.symbol == symbol && entry.accepts(type); });

    if (found == unaryOperators.end())
        return std::nullopt;

    return found->op;
}

Op findBinaryOp(char symbol, ValueType type)
{
    auto found = std::ranges::find(binaryOperators, symbol, &BinaryOperator::symbol);

    if (found == binaryOperators.end())
        return noBinaryOp;

    if (isFloatFamily(type))
        return found->floatOp;

    if (isSignedInteger(type))
        return found->signedOp;

    return isUnsignedInteger(type) ? found->unsignedOp : noBinaryOp;
}

Op getConversionToFloat(ValueType source)
{
    if (isUnsignedInteger(source))
        return Op::FloatFromU;

    if (isSignedInteger(source))
        return Op::FloatFromS;

    return isBoolean(source) ? Op::FloatFromMask : Op::CopyBits;
}

Op getConversionToInteger(ValueType source, ValueType target)
{
    if (isFloatFamily(source))
        return isSignedInteger(target) ? Op::IntFromF : Op::UIntFromF;

    return isBoolean(source) ? Op::IntFromMask : Op::CopyBits;
}

std::optional<Op> findMinMaxOp(ValueType type, bool isMin)
{
    if (isFloatVector(type))
        return isMin ? Op::MinF : Op::MaxF;

    if (isSignedInteger(type))
        return isMin ? Op::MinS : Op::MaxS;

    if (isUnsignedInteger(type))
        return isMin ? Op::MinU : Op::MaxU;

    return std::nullopt;
}

Op getComparisonOp(ValueType operand)
{
    if (isFloatFamily(operand))
        return Op::CmpF;

    return isSignedInteger(operand) ? Op::CmpS : Op::CmpU;
}

bool isSwizzleShape(std::size_t count, const Plan::Node& node, ValueType source)
{
    return count > 0 && count <= node.swizzle.size()
           && static_cast<int>(count) == node.components && !isMatrix(source);
}

bool writeSwizzleComponents(Plan::Node& node, std::string_view letters, int width)
{
    for (auto at = std::size_t {0}; at < letters.size(); ++at)
    {
        auto component = std::string_view("xyzw").find(letters[at]);

        if (component == std::string_view::npos
            || static_cast<int>(component) >= width)
            return false;

        node.swizzle[at] = static_cast<std::uint8_t>(component);
    }

    return true;
}

bool isBitcast(ValueType source, ValueType target)
{
    return componentCount(source) == componentCount(target) && !isBoolean(source)
           && !isBoolean(target);
}

bool isConversion(std::string_view name, ValueType source, ValueType target)
{
    return name == typeName(target)
           && componentCount(source) == componentCount(target) && !isMatrix(source);
}
} // namespace

Vector<int> PlanBuilder::collectReachableRoots() const
{
    auto roots = Vector<int> {};

    for (const auto& schedule: pending)
        for (auto root: schedule.roots)
            if (root >= 0)
                roots.add(root);

    return roots;
}

void PlanBuilder::markReachable()
{
    auto stack = collectReachableRoots();

    while (!stack.empty() && !failed())
    {
        auto id = stack.back();
        stack.pop_back();

        if (!markNodeReachable(id, stack))
            return;
    }
}

bool PlanBuilder::markNodeReachable(NodeId id, Vector<int>& stack)
{
    if (plan.nodes[id].used)
        return true;

    plan.nodes[id].used = true;
    const auto& expr = graph.expr(id);

    if (!pushReachable(expr.args, stack))
        return false;

    return expr.kind != ExprKind::ArrayRead || markArrayReachable(expr.index, stack);
}

bool PlanBuilder::markArrayReachable(int array, Vector<int>& stack)
{
    if (array < 0 || array >= graph.arrays().size())
    {
        fail("an ArrayRead names array {}, which does not exist", array);
        return false;
    }

    if (arrayUsed[array] != 0)
        return true;

    arrayUsed[array] = 1;
    return pushReachable(graph.arrays()[array].elements, stack);
}

bool PlanBuilder::pushReachable(const Vector<int>& ids, Vector<int>& stack)
{
    for (auto id: ids)
    {
        if (!checkNode(id))
            return false;

        stack.add(id);
    }

    return true;
}

ValueType PlanBuilder::getArgumentType(const Expr& expr, int which) const
{
    return graph.expr(expr.args[which]).type;
}

int PlanBuilder::getArgumentComponents(const Expr& expr, int which) const
{
    return componentCount(getArgumentType(expr, which));
}

void PlanBuilder::rejectNodeWith(NodeId id, std::string_view why)
{
    const auto& expr = graph.expr(id);
    auto kind = Miro::enumToString(expr.kind);

    if (expr.kind != ExprKind::Call)
    {
        fail("expression {} (node {}): {}", kind, id, why);
        return;
    }

    fail("expression {} \"{}\" (node {}): {}", kind, expr.text, id, why);
}

bool PlanBuilder::requireArguments(NodeId id, int count)
{
    if (graph.expr(id).args.size() == count)
        return true;

    rejectNode(id, "expected {} arguments", count);
    return false;
}

bool PlanBuilder::requireBroadcastable(NodeId id)
{
    const auto& expr = graph.expr(id);
    auto width = componentCount(expr.type);

    for (auto which = 0; which < expr.args.size(); ++which)
    {
        auto components = getArgumentComponents(expr, which);

        if (components != width && components != 1)
        {
            rejectNode(id, "an argument's width does not match the result");
            return false;
        }
    }

    return true;
}

bool PlanBuilder::requireScalarIndex(NodeId id, const Expr& expr)
{
    if (isScalarInteger(getArgumentType(expr, 0)))
        return true;

    rejectNode(id, "the index is not a scalar integer");
    return false;
}

bool PlanBuilder::requireUnaryFloatVector(NodeId id, const Expr& expr)
{
    if (!requireArguments(id, 1))
        return false;

    if (isFloatVector(expr.type) && getArgumentType(expr, 0) == expr.type)
        return true;

    rejectNode(id, "defined on the float vectors only");
    return false;
}

void PlanBuilder::decodeNode(NodeId id)
{
    using Entry = std::pair<ExprKind, NodeDecoder>;

    static constexpr auto decoders = Array<Entry, 26> {
        Entry {ExprKind::Input, &PlanBuilder::rejectRenderInput},
        Entry {ExprKind::Varying, &PlanBuilder::rejectRenderInput},
        Entry {ExprKind::BuiltIn, &PlanBuilder::rejectRenderInput},
        Entry {ExprKind::Sample, &PlanBuilder::rejectTexture},
        Entry {ExprKind::Fetch, &PlanBuilder::rejectTexture},
        Entry {ExprKind::AtomicLoad, &PlanBuilder::decodeAtomicLoad},
        Entry {ExprKind::LocalId, &PlanBuilder::decodeIdLeaf},
        Entry {ExprKind::GroupId, &PlanBuilder::decodeIdLeaf},
        Entry {ExprKind::ThreadId, &PlanBuilder::decodeIdLeaf},
        Entry {ExprKind::SharedRead, &PlanBuilder::decodeSharedRead},
        Entry {ExprKind::SimdGroupIndex, &PlanBuilder::decodeLeaf},
        Entry {ExprKind::Uniform, &PlanBuilder::decodeUniform},
        Entry {ExprKind::Constant, &PlanBuilder::decodeLeaf},
        Entry {ExprKind::GridExtent, &PlanBuilder::decodeLeaf},
        Entry {ExprKind::VarRead, &PlanBuilder::decodeVarRead},
        Entry {ExprKind::Construct, &PlanBuilder::decodeConstruct},
        Entry {ExprKind::Swizzle, &PlanBuilder::decodeSwizzle},
        Entry {ExprKind::Call, &PlanBuilder::decodeCall},
        Entry {ExprKind::Unary, &PlanBuilder::decodeUnary},
        Entry {ExprKind::Binary, &PlanBuilder::decodeBinary},
        Entry {ExprKind::Compare, &PlanBuilder::decodeCompare},
        Entry {ExprKind::Select, &PlanBuilder::decodeSelect},
        Entry {ExprKind::Mul, &PlanBuilder::decodeMul},
        Entry {ExprKind::BufferRead, &PlanBuilder::decodeBufferRead},
        Entry {ExprKind::BufferVectorRead, &PlanBuilder::decodeBufferRead},
        Entry {ExprKind::ArrayRead, &PlanBuilder::decodeArrayRead}};

    const auto& expr = graph.expr(id);
    auto& node = plan.nodes[id];
    node.components = static_cast<std::uint8_t>(componentCount(expr.type));
    node.argBegin = plan.arguments.size();
    node.argCount = expr.args.size();

    for (auto argument: expr.args)
        plan.arguments.add(argument);

    auto found = std::ranges::find(decoders, expr.kind, &Entry::first);
    auto decoder = found->second;
    (this->*decoder)(id, node, expr);
}

void PlanBuilder::rejectRenderInput(NodeId id, Plan::Node&, const Expr&)
{
    rejectNode(id, "a render graph input cannot run as a kernel");
}

void PlanBuilder::rejectTexture(NodeId id, Plan::Node&, const Expr&)
{
    rejectNode(id, "textures are not supported by the CPU executor");
}

void PlanBuilder::decodeIdLeaf(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!isThreadIdShape(expr.index, node.components))
    {
        rejectNode(id, getMissingAxisReason(expr.kind));
        return;
    }

    if (expr.kind == ExprKind::GroupId)
        plan.groupIdLeaves.add({id, expr.index});
    else if (expr.kind == ExprKind::ThreadId)
        plan.threadIdLeaves.add({id, expr.index});
}

void PlanBuilder::decodeLeaf(NodeId id, Plan::Node&, const Expr& expr)
{
    if (expr.kind == ExprKind::SimdGroupIndex)
        plan.simdGroupLeaves.add(id);
    else if (expr.kind == ExprKind::Constant)
        plan.constantNodes.add({id, getConstantWord(expr)});
    else
        plan.extentLeaves.add({id, expr.index});
}

void PlanBuilder::decodeUniform(NodeId id, Plan::Node&, const Expr& expr)
{
    if (expr.index < 0 || expr.index >= plan.uniformTypes.size())
    {
        rejectNode(id, "names a uniform slot that does not exist");
        return;
    }

    plan.uniformLeaves.add({id, expr.index});
}

void PlanBuilder::decodeVarRead(NodeId id, Plan::Node&, const Expr& expr)
{
    if (expr.index < 0 || expr.index >= graph.variables().size())
        rejectNode(id, "names a variable that does not exist");
}

void PlanBuilder::decodeSelect(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 3))
        return;

    if (getArgumentType(expr, 0) != ValueType::Bool)
    {
        rejectNode(id, "the condition is not a scalar Bool");
        return;
    }

    if (requireBroadcastable(id))
        node.op = Op::Select;
}

void PlanBuilder::decodeBufferRead(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 1) || !checkSlot(expr.index))
        return;

    if (isMatrix(expr.type) || isBoolean(expr.type))
    {
        rejectNode(id, "reads a type no buffer holds");
        return;
    }

    node.op =
        expr.kind == ExprKind::BufferRead ? Op::BufferRead : Op::BufferVectorRead;
    node.immediate = expr.index;
}

void PlanBuilder::decodeArrayRead(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 1))
        return;

    if (graph.arrays()[expr.index].elementType != expr.type)
    {
        rejectNode(id, "the element type does not match the array");
        return;
    }

    node.op = Op::ArrayRead;
    node.immediate = expr.index;
}

void PlanBuilder::decodeAtomicLoad(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 1) || !checkSlot(expr.index))
        return;

    if (!requireScalarIndex(id, expr))
        return;

    if (plan.slotAccess[static_cast<std::size_t>(expr.index)] != BufferAccess::Atomic
        || expr.type != ValueType::UInt)
    {
        rejectNode(id, "reads a slot that is not atomic");
        return;
    }

    node.op = Op::AtomicLoad;
    node.immediate = expr.index;
}

void PlanBuilder::decodeSharedRead(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 1) || !requireScalarIndex(id, expr))
        return;

    if (expr.index < 0 || expr.index >= graph.sharedArrays().size())
    {
        rejectNode(id, "names a shared array that does not exist");
        return;
    }

    if (graph.sharedArrays()[expr.index].elementType != expr.type)
    {
        rejectNode(id, "the element type does not match the shared array");
        return;
    }

    node.op = Op::SharedRead;
    node.immediate = expr.index;
}

Word PlanBuilder::getConstantWord(const Expr& expr)
{
    switch (expr.type)
    {
        case ValueType::Float:
            return Lanes::toWord(expr.value);
        case ValueType::Bool:
            return Lanes::maskOf(expr.index != 0);
        default:
            return static_cast<Word>(expr.index);
    }
}

void PlanBuilder::decodeConstruct(NodeId id, Plan::Node& node, const Expr& expr)
{
    auto total = 0;

    for (auto which = 0; which < expr.args.size(); ++which)
        total += getArgumentComponents(expr, which);

    if (total != node.components || expr.args.empty())
    {
        rejectNode(id,
                   "its arguments hold {} components where the result has {}",
                   total,
                   node.components);
        return;
    }

    node.op = Op::Construct;
}

void PlanBuilder::decodeSwizzle(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 1))
        return;

    const auto& letters = expr.text;
    auto source = getArgumentType(expr, 0);

    if (!isSwizzleShape(letters.size(), node, source)
        || !writeSwizzleComponents(node, letters, componentCount(source)))
    {
        rejectNode(id, "malformed component list \"{}\"", letters);
        return;
    }

    node.op = Op::Swizzle;
}

void PlanBuilder::decodeUnary(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 1))
        return;

    auto op = findUnaryOp(expr.op, expr.type);

    if (op)
    {
        node.op = *op;
        return;
    }

    rejectNode(id, "unsupported operator '{}' on {}", expr.op, typeName(expr.type));
}

void PlanBuilder::decodeBinary(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 2) || !requireBroadcastable(id))
        return;

    if (!expr.text.empty())
    {
        decodeShift(id, node, expr);
        return;
    }

    auto op = findBinaryOp(expr.op, expr.type);

    if (op != noBinaryOp)
    {
        node.op = op;
        return;
    }

    rejectNode(id, "unsupported operator '{}' on {}", expr.op, typeName(expr.type));
}

void PlanBuilder::decodeShift(NodeId id, Plan::Node& node, const Expr& expr)
{
    auto type = expr.type;

    if (!isIntegerFamily(type) || !isShiftOperator(expr.text))
    {
        rejectNode(
            id, "unsupported operator \"{}\" on {}", expr.text, typeName(type));
        return;
    }

    if (expr.text == "<<")
        node.op = Op::Shl;
    else
        node.op = isSignedInteger(type) ? Op::ShrS : Op::ShrU;
}

void PlanBuilder::decodeCompare(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 2) || !requireBroadcastable(id))
        return;

    if (!isBoolean(expr.type))
    {
        rejectNode(id, "the result is not a Bool");
        return;
    }

    auto operandType = getArgumentType(expr, 0);

    if (isBoolean(operandType))
    {
        decodeBoolCompare(id, node, expr);
        return;
    }

    auto text = std::string_view {expr.text};
    auto found = std::ranges::find(relationNames, text, &RelationName::text);

    if (found == relationNames.end() || isMatrix(operandType))
    {
        rejectNode(
            id, "unsupported relation \"{}\" on {}", text, typeName(operandType));
        return;
    }

    node.sub = static_cast<std::uint8_t>(found->relation);
    node.op = getComparisonOp(operandType);
}

void PlanBuilder::decodeBoolCompare(NodeId id, Plan::Node& node, const Expr& expr)
{
    auto text = std::string_view {expr.text};
    auto found = std::ranges::find(boolRelations, text, &BoolRelation::text);

    if (found == boolRelations.end())
    {
        rejectNode(id, "unsupported relation \"{}\" on a Bool", text);
        return;
    }

    node.op = found->op;
}

void PlanBuilder::decodeMul(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 2))
        return;

    auto left = getArgumentType(expr, 0);
    auto right = getArgumentType(expr, 1);
    auto leftOrder = matrixOrder(left);
    auto rightOrder = matrixOrder(right);

    if (isVectorFor(right, leftOrder))
    {
        node.op = Op::MatVec;
        node.order = static_cast<std::uint8_t>(leftOrder);
    }
    else if (isVectorFor(left, rightOrder))
    {
        node.op = Op::VecMat;
        node.order = static_cast<std::uint8_t>(rightOrder);
    }
    else if (leftOrder > 0 && leftOrder == rightOrder)
    {
        node.op = Op::MatMat;
        node.order = static_cast<std::uint8_t>(leftOrder);
    }
    else
    {
        rejectNode(
            id, "unsupported product of {} and {}", typeName(left), typeName(right));
    }
}

void PlanBuilder::decodeCall(NodeId id, Plan::Node& node, const Expr& expr)
{
    const auto& name = expr.text;

    if (derivativeNames.contains(name))
        rejectNode(id, "derivatives exist only in a fragment shader");
    else if (name.starts_with("eacp"))
        decodeHelper(id, node, expr);
    else if (expr.args.empty())
        rejectNode(id, "a call with no arguments");
    else if (!decodeNamedCall(id, node, expr))
        decodeConversionCall(id, node, expr);
}

void PlanBuilder::decodeConversionCall(NodeId id, Plan::Node& node, const Expr& expr)
{
    const auto& name = expr.text;

    if (name.starts_with("as_type<"))
        decodeBitcast(id, node, expr);
    else if (isConversionName(name))
        decodeConversion(id, node, expr);
    else
        rejectNode(id, "unknown builtin");
}

bool PlanBuilder::decodeNamedCall(NodeId id, Plan::Node& node, const Expr& expr)
{
    using Entry = std::pair<std::string_view, NodeDecoder>;

    static constexpr auto decoders =
        Array<Entry, 7> {Entry {"abs", &PlanBuilder::decodeAbs},
                         Entry {"min", &PlanBuilder::decodeMinMax},
                         Entry {"max", &PlanBuilder::decodeMinMax},
                         Entry {"transpose", &PlanBuilder::decodeMatrixFunction},
                         Entry {"determinant", &PlanBuilder::decodeMatrixFunction},
                         Entry {"all", &PlanBuilder::decodeBoolReduction},
                         Entry {"any", &PlanBuilder::decodeBoolReduction}};

    auto name = std::string_view {expr.text};

    if (auto math = std::ranges::find(mathNames, name, &MathName::name);
        math != mathNames.end())
    {
        decodeMath(id, node, expr, math->function);
        return true;
    }

    if (auto broadcast = std::ranges::find(broadcastNames, name, &NamedOp::name);
        broadcast != broadcastNames.end())
    {
        if (requireArguments(id, broadcast->arguments) && requireBroadcastable(id))
            decodeBroadcast(id, node, expr, broadcast->op);

        return true;
    }

    if (auto geometric = std::ranges::find(geometricNames, name, &NamedOp::name);
        geometric != geometricNames.end())
    {
        if (requireArguments(id, geometric->arguments))
            decodeGeometric(id, node, expr, geometric->op);

        return true;
    }

    auto found = std::ranges::find(decoders, name, &Entry::first);

    if (found == decoders.end())
        return false;

    auto decoder = found->second;
    (this->*decoder)(id, node, expr);
    return true;
}

void PlanBuilder::decodeMath(NodeId id,
                             Plan::Node& node,
                             const Expr& expr,
                             MathFunction function)
{
    if (!requireUnaryFloatVector(id, expr))
        return;

    node.op = Op::UnaryMath;
    node.sub = static_cast<std::uint8_t>(function);
}

void PlanBuilder::decodeBroadcast(NodeId id,
                                  Plan::Node& node,
                                  const Expr& expr,
                                  Op op)
{
    if (isFloatVector(expr.type))
        node.op = op;
    else
        rejectNode(id, "defined on the float vectors only");
}

void PlanBuilder::decodeAbs(NodeId id, Plan::Node& node, const Expr& expr)
{
    auto type = expr.type;

    if (!requireArguments(id, 1))
        return;

    if (getArgumentType(expr, 0) != type || isMatrix(type))
    {
        rejectNode(id, "the argument and the result differ");
    }
    else if (isFloatFamily(type))
    {
        node.op = Op::UnaryMath;
        node.sub = static_cast<std::uint8_t>(MathFunction::Abs);
    }
    else if (isSignedInteger(type))
    {
        node.op = Op::AbsS;
    }
    else
    {
        rejectNode(id, "unsupported on {}", typeName(type));
    }
}

void PlanBuilder::decodeMinMax(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 2) || !requireBroadcastable(id))
        return;

    auto op = findMinMaxOp(expr.type, expr.text == "min");

    if (!op)
    {
        rejectNode(id, "unsupported on {}", typeName(expr.type));
        return;
    }

    node.op = *op;
}

void PlanBuilder::decodeMatrixFunction(NodeId id, Plan::Node& node, const Expr& expr)
{
    auto operand = getArgumentType(expr, 0);

    if (!requireArguments(id, 1) || !isMatrix(operand))
    {
        rejectNode(id, "takes a matrix");
        return;
    }

    node.op = expr.text == "transpose" ? Op::Transpose : Op::Determinant;
    node.order = static_cast<std::uint8_t>(matrixOrder(operand));
}

void PlanBuilder::decodeBoolReduction(NodeId id, Plan::Node& node, const Expr& expr)
{
    auto operand = getArgumentType(expr, 0);

    if (!requireArguments(id, 1))
        return;

    if (!isBoolean(operand) || expr.type != ValueType::Bool)
    {
        rejectNode(id, "takes a Bool vector");
        return;
    }

    node.op = expr.text == "all" ? Op::All : Op::Any;
    node.order = static_cast<std::uint8_t>(componentCount(operand));
}

void PlanBuilder::decodeBitcast(NodeId id, Plan::Node& node, const Expr& expr)
{
    if (!requireArguments(id, 1))
        return;

    if (!isBitcast(getArgumentType(expr, 0), expr.type))
    {
        rejectNode(id, "the bitcast changes the width");
        return;
    }

    node.op = Op::CopyBits;
}

void PlanBuilder::decodeHelper(NodeId id, Plan::Node& node, const Expr& expr)
{
    auto name = std::string_view {expr.text};
    auto helper = std::ranges::find(componentHelperNames, name, &HelperName::name);

    if (helper == componentHelperNames.end())
    {
        decodePackedHelper(id, node, expr);
        return;
    }

    if (requireUnaryFloatVector(id, expr))
    {
        node.op = Op::Helper;
        node.sub = static_cast<std::uint8_t>(helper->function);
    }
}

void PlanBuilder::decodePackedHelper(NodeId id, Plan::Node& node, const Expr& expr)
{
    auto name = std::string_view {expr.text};
    auto packed =
        std::ranges::find(packedHelperNames, name, &PackedHelperName::name);

    if (packed == packedHelperNames.end())
    {
        rejectNode(id, "unknown helper");
        return;
    }

    const auto& signature = packed->signature;

    if (!requireArguments(id, signature.arguments))
        return;

    auto matches = expr.type == signature.result
                   && getArgumentType(expr, 0) == signature.first
                   && (signature.arguments == 1
                       || getArgumentType(expr, 1) == signature.second);

    if (!matches)
    {
        rejectNode(id, "its argument or result types differ from the helper's");
        return;
    }

    node.op = Op::Helper;
    node.sub = static_cast<std::uint8_t>(packed->function);
}

void PlanBuilder::decodeGeometric(NodeId id,
                                  Plan::Node& node,
                                  const Expr& expr,
                                  Op op)
{
    auto operand = getArgumentType(expr, 0);
    auto width = componentCount(operand);

    if (!isFloatVector(operand) || width < 2)
    {
        rejectNode(id, "takes float vectors");
        return;
    }

    if (!hasGeometricWidths(expr, op, width))
    {
        rejectNode(id, "the arguments' widths differ");
        return;
    }

    if (op == Op::Cross && width != 3)
    {
        rejectNode(id, "takes Float3s");
        return;
    }

    node.op = op;
    node.order = static_cast<std::uint8_t>(width);
}

// Refract's third argument is its scalar eta; every other argument is a float
// vector of the first's width.
bool PlanBuilder::hasGeometricWidths(const Expr& expr, Op op, int width) const
{
    for (auto which = 1; which < expr.args.size(); ++which)
    {
        auto isEta = op == Op::Refract && which == 2;
        auto expected = isEta ? 1 : width;

        if (getArgumentComponents(expr, which) != expected
            || !isFloatFamily(getArgumentType(expr, which)))
            return false;
    }

    return true;
}

void PlanBuilder::decodeConversion(NodeId id, Plan::Node& node, const Expr& expr)
{
    auto source = getArgumentType(expr, 0);
    auto target = expr.type;

    if (!requireArguments(id, 1))
        return;

    if (!isConversion(expr.text, source, target))
    {
        rejectNode(id, "the conversion changes the width");
        return;
    }

    if (isFloatFamily(target))
        node.op = getConversionToFloat(source);
    else
        node.op = getConversionToInteger(source, target);
}
} // namespace eacp::GPU::CpuCompute
