#include "ShaderGraph.h"

#include <utility>

namespace eacp::GPU
{
int ShaderGraph::add(Expr node)
{
    nodes.add(std::move(node));
    return nodes.size() - 1;
}

int ShaderGraph::addInput(ValueType type)
{
    auto node = Expr {};
    node.kind = ExprKind::Input;
    node.type = type;
    node.index = inputTypes.size();
    inputTypes.add(type);
    return add(std::move(node));
}

int ShaderGraph::addVarying(ValueType type, int sourceNode)
{
    auto node = Expr {};
    node.kind = ExprKind::Varying;
    node.type = type;
    node.index = varyingSlots.size();
    varyingSlots.add({type, sourceNode});
    return add(std::move(node));
}

int ShaderGraph::addConstant(float value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::Float;
    node.value = value;
    return add(std::move(node));
}

int ShaderGraph::addConstruct(ValueType type, Vector<int> args)
{
    auto node = Expr {};
    node.kind = ExprKind::Construct;
    node.type = type;
    node.args = std::move(args);
    return add(std::move(node));
}

int ShaderGraph::addSwizzle(ValueType type, int child, std::string components)
{
    auto node = Expr {};
    node.kind = ExprKind::Swizzle;
    node.type = type;
    node.args.add(child);
    node.swizzle = std::move(components);
    return add(std::move(node));
}

int ShaderGraph::addBinary(ValueType type, char op, int lhs, int rhs)
{
    auto node = Expr {};
    node.kind = ExprKind::Binary;
    node.type = type;
    node.op = op;
    node.args.add(lhs);
    node.args.add(rhs);
    return add(std::move(node));
}
} // namespace eacp::GPU
