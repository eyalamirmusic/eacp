#include "Graph.h"

namespace eacp::ML
{
namespace
{
using GPU::ExprKind;
using GPU::ValueType;

const char* exprKindName(ExprKind kind)
{
    switch (kind)
    {
        case ExprKind::Input:
            return "Input";
        case ExprKind::Varying:
            return "Varying";
        case ExprKind::Uniform:
            return "Uniform";
        case ExprKind::Constant:
            return "Constant";
        case ExprKind::Construct:
            return "Construct";
        case ExprKind::Swizzle:
            return "Swizzle";
        case ExprKind::Call:
            return "Call";
        case ExprKind::Unary:
            return "Unary";
        case ExprKind::Binary:
            return "Binary";
        case ExprKind::Compare:
            return "Compare";
        case ExprKind::Select:
            return "Select";
        default:
            return "a node kind";
    }
}

std::string_view binaryOpName(char op)
{
    switch (op)
    {
        case '+':
            return "add";
        case '-':
            return "sub";
        case '*':
            return "mul";
        case '/':
            return "real_div";
        default:
            return {};
    }
}

std::string_view unaryCallName(std::string_view call)
{
    if (call == "exp" || call == "tanh" || call == "sqrt" || call == "abs"
        || call == "floor")
        return call;

    if (call == "eacpErf")
        return "erf";

    return {};
}

std::string_view binaryCallName(std::string_view call)
{
    if (call == "max")
        return "maximum";

    if (call == "min")
        return "minimum";

    if (call == "pow")
        return "pow";

    return {};
}

std::string_view compareOpName(std::string_view op)
{
    if (op == "<")
        return "less";
    if (op == "<=")
        return "less_equal";
    if (op == ">")
        return "greater";
    if (op == ">=")
        return "greater_equal";
    if (op == "==")
        return "equal";
    if (op == "!=")
        return "not_equal";
    if (op == "&&")
        return "logical_and";
    if (op == "||")
        return "logical_or";

    return {};
}

bool isLogicalOp(std::string_view op)
{
    return op == "logical_and" || op == "logical_or";
}
} // namespace

// One apply's walk of the shader expression its body recorded, from the value
// it returned down to its operands. A node lowers to a tensor of the graph, or
// to a scalar still waiting for the dtype of whatever op consumes it.
class Graph::ElementwiseLowering
{
public:
    ElementwiseLowering(Graph& graphToUse,
                        const Vector<Tensor>& operandsToUse,
                        int firstSlotToUse,
                        MIL::DataType typeToUse)
        : graph(graphToUse)
        , operands(operandsToUse)
        , firstSlot(firstSlotToUse)
        , type(typeToUse)
        , shaderGraph(graphToUse.builder.graph())
    {
        memo.resize(shaderGraph.nodeCount());
    }

    Tensor lower(int root)
    {
        auto result = lowerNode(root);

        if (failed)
            return {};

        if (result.isScalar || !result.dependsOnTensor)
            return graph.fail("apply", "the result does not depend on any tensor");

        if (graph.node(result.tensor).type != type)
            return graph.fail("apply", "the result is not a floating-point value");

        return result.tensor;
    }

private:
    struct Operand
    {
        Tensor tensor;
        bool isScalar = false;
        float value = 0.f;
        bool dependsOnTensor = true;
    };

    Operand refuse(const std::string& message)
    {
        if (!failed)
            graph.fail("apply", message);

        failed = true;
        return {};
    }

    Operand lowerNode(int index)
    {
        if (memo[index])
            return *memo[index];

        auto lowered = lowerUncached(shaderGraph.expr(index));
        memo[index] = lowered;
        return lowered;
    }

    Operand lowerUncached(const GPU::Expr& expr)
    {
        if (failed)
            return {};

        auto expected =
            expr.kind == ExprKind::Compare ? ValueType::Bool : ValueType::Float;

        if (expr.type != expected && !isLogicalNot(expr))
            return refuse(std::string {exprKindName(expr.kind)}
                          + " of a non-scalar or non-float type");

        switch (expr.kind)
        {
            case ExprKind::Input:
                return lowerInput(expr);
            case ExprKind::Constant:
                return {{}, true, expr.value};
            case ExprKind::Binary:
                return lowerBinary(expr);
            case ExprKind::Unary:
                return lowerUnary(expr);
            case ExprKind::Call:
                return lowerCall(expr);
            case ExprKind::Compare:
                return lowerCompare(expr);
            case ExprKind::Select:
                return lowerSelect(expr);
            default:
                return refuse(std::string {exprKindName(expr.kind)}
                              + " has no MIL lowering");
        }
    }

    static bool isLogicalNot(const GPU::Expr& expr)
    {
        return expr.kind == ExprKind::Unary && expr.op == '!'
               && expr.type == ValueType::Bool;
    }

    Operand lowerInput(const GPU::Expr& expr)
    {
        auto operand = expr.index - firstSlot;

        if (operand < 0 || operand >= operands.size())
            return refuse("a value from outside this apply was used");

        auto tensor = operands[operand];
        auto& node = graph.node(tensor);

        if (node.kind == NodeKind::scalar)
            return {{}, true, node.value};

        return {tensor};
    }

    Operand lowerBinary(const GPU::Expr& expr)
    {
        auto name = binaryOpName(expr.op);

        if (!expr.text.empty() || name.empty())
            return refuse("Binary operator '"
                          + (expr.text.empty() ? std::string {expr.op} : expr.text)
                          + "' has no MIL lowering");

        return emit(name, {{"x", expr.args[0]}, {"y", expr.args[1]}}, type);
    }

    Operand lowerUnary(const GPU::Expr& expr)
    {
        if (isLogicalNot(expr))
            return emit(
                "logical_not", {{"x", expr.args[0]}}, MIL::DataType::boolean);

        if (expr.op != '-')
            return refuse("Unary operator '" + std::string {expr.op}
                          + "' has no MIL lowering");

        auto child = lowerNode(expr.args[0]);

        if (child.isScalar)
            return {{}, true, -child.value};

        return emitOperands("mul", {{"x", child}, {"y", {{}, true, -1.f}}}, type);
    }

    Operand lowerCall(const GPU::Expr& expr)
    {
        auto arity = expr.args.size();

        if (auto epsilon = requiredEpsilon(expr.text); epsilon && arity == 1)
            return emitWithEpsilon(expr.text, expr.args[0], *epsilon);

        if (auto name = unaryCallName(expr.text); !name.empty() && arity == 1)
            return emit(name, {{"x", expr.args[0]}}, type);

        if (auto name = binaryCallName(expr.text); !name.empty() && arity == 2)
            return emit(name, {{"x", expr.args[0]}, {"y", expr.args[1]}}, type);

        if (expr.text == "clamp" && arity == 3)
            return lowerClamp(expr);

        return refuse("Call '" + expr.text + "' has no MIL lowering");
    }

    // Core ML's parser requires the epsilon coremltools documents as optional;
    // these are coremltools' defaults.
    static std::optional<float> requiredEpsilon(std::string_view call)
    {
        if (call == "log")
            return 1e-45f;

        if (call == "rsqrt")
            return 1e-12f;

        return std::nullopt;
    }

    Operand emitWithEpsilon(std::string_view op, int argument, float epsilon)
    {
        auto value = lowerNode(argument);

        if (failed)
            return {};

        return emitOperands(
            op, {{"x", value}, {"epsilon", {{}, true, epsilon}}}, type);
    }

    Operand lowerClamp(const GPU::Expr& expr)
    {
        auto value = lowerNode(expr.args[0]);
        auto low = lowerNode(expr.args[1]);
        auto high = lowerNode(expr.args[2]);

        if (failed)
            return {};

        if (low.isScalar && high.isScalar && !value.isScalar)
            return emitOperands(
                "clip", {{"x", value}, {"alpha", low}, {"beta", high}}, type);

        auto raised = emitOperands("maximum", {{"x", value}, {"y", low}}, type);
        return emitOperands("minimum", {{"x", raised}, {"y", high}}, type);
    }

    Operand lowerCompare(const GPU::Expr& expr)
    {
        auto name = compareOpName(expr.text);

        if (name.empty())
            return refuse("Compare '" + expr.text + "' has no MIL lowering");

        auto operandType = isLogicalOp(name) ? ValueType::Bool : ValueType::Float;

        for (auto arg: expr.args)
            if (shaderGraph.expr(arg).type != operandType)
                return refuse("Compare '" + expr.text + "' of mismatched types");

        return emit(name,
                    {{"x", expr.args[0]}, {"y", expr.args[1]}},
                    MIL::DataType::boolean);
    }

    Operand lowerSelect(const GPU::Expr& expr)
    {
        if (shaderGraph.expr(expr.args[0]).type != ValueType::Bool)
            return refuse("Select needs a Bool condition");

        return emit(
            "select",
            {{"cond", expr.args[0]}, {"a", expr.args[1]}, {"b", expr.args[2]}},
            type);
    }

    struct Argument
    {
        std::string_view parameter;
        int node = -1;
    };

    struct LoweredArgument
    {
        std::string_view parameter;
        Operand operand;
    };

    Operand emit(std::string_view op,
                 std::initializer_list<Argument> arguments,
                 MIL::DataType resultType)
    {
        auto lowered = Vector<LoweredArgument> {};

        for (auto& argument: arguments)
            lowered.add({argument.parameter, lowerNode(argument.node)});

        if (failed)
            return {};

        return emitOperands(op, lowered, resultType);
    }

    Operand emitOperands(std::string_view op,
                         const Vector<LoweredArgument>& arguments,
                         MIL::DataType resultType)
    {
        if (failed)
            return {};

        auto parameters = Vector<Parameter> {};
        auto shape = Shape {};
        auto dependsOnTensor = false;

        for (auto& argument: arguments)
        {
            if (argument.operand.isScalar)
            {
                parameters.add(valueParameter(
                    argument.parameter,
                    MIL::Value::scalar(argument.operand.value, type)));
                continue;
            }

            dependsOnTensor = dependsOnTensor || argument.operand.dependsOnTensor;

            auto broadcast = graph.broadcastShape(
                shape, graph.node(argument.operand.tensor).shape);

            if (!broadcast)
                return refuse("operand shapes do not broadcast");

            shape = *broadcast;
            parameters.add(
                tensorParameter(argument.parameter, argument.operand.tensor));
        }

        auto result = graph.addOperation(op, shape, resultType, parameters);
        return {result, false, 0.f, dependsOnTensor};
    }

    Graph& graph;
    const Vector<Tensor>& operands;
    int firstSlot = 0;
    MIL::DataType type = MIL::DataType::float32;
    const GPU::ShaderGraph& shaderGraph;
    Vector<std::optional<Operand>> memo;
    bool failed = false;
};

Tensor Graph::apply(const Vector<Tensor>& operands, const ElementwiseBody& body)
{
    if (operands.empty())
        return fail("apply", "needs at least one operand");

    for (auto operand: operands)
        if (!acceptOperands("apply", {operand}))
            return {};

    auto type = std::optional<MIL::DataType> {};
    auto shape = Shape {};

    for (auto operand: operands)
    {
        auto& operandNode = node(operand);

        if (operandNode.kind == NodeKind::scalar)
            continue;

        if (!isFloat(operand) || (type && *type != operandNode.type))
            return fail("apply", "operands need one floating-point type");

        auto broadcast = broadcastShape(shape, operandNode.shape);

        if (!broadcast)
            return fail("apply",
                        "operand " + operandNode.shape.toString()
                            + " does not broadcast against " + shape.toString());

        type = operandNode.type;
        shape = *broadcast;
    }

    auto& shaderGraph = builder.graph();
    auto firstSlot = shaderGraph.inputs().size();
    auto statementsBefore =
        shaderGraph.block(GPU::ShaderGraph::rootBlock).statements.size();

    auto values = Vector<GPU::Float> {};

    for (auto index = 0; index < operands.size(); ++index)
        values.add(builder.vertexInput<GPU::Float>());

    auto result = body(values);

    if (result.graph != &shaderGraph || result.node < 0)
        return fail("apply",
                    "the body did not return a value built from its operands");

    if (shaderGraph.block(GPU::ShaderGraph::rootBlock).statements.size()
        != statementsBefore)
        return fail("apply", "the body recorded a statement");

    auto lowering = ElementwiseLowering {
        *this, operands, firstSlot, type.value_or(MIL::DataType::float32)};

    return lowering.lower(result.node);
}
} // namespace eacp::ML
