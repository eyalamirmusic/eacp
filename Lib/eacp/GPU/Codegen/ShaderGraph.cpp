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

int ShaderGraph::addUniform(ValueType type)
{
    auto node = Expr {};
    node.kind = ExprKind::Uniform;
    node.type = type;
    node.index = uniformTypes.size();
    uniformTypes.add(type);
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
    node.text = std::move(components);
    return add(std::move(node));
}

int ShaderGraph::addCall(ValueType type, std::string name, int argument)
{
    auto node = Expr {};
    node.kind = ExprKind::Call;
    node.type = type;
    node.args.add(argument);
    node.text = std::move(name);
    return add(std::move(node));
}

int ShaderGraph::addCall(ValueType type, std::string name, Vector<int> args)
{
    auto node = Expr {};
    node.kind = ExprKind::Call;
    node.type = type;
    node.args = std::move(args);
    node.text = std::move(name);
    return add(std::move(node));
}

int ShaderGraph::addUnary(ValueType type, char op, int child)
{
    auto node = Expr {};
    node.kind = ExprKind::Unary;
    node.type = type;
    node.op = op;
    node.args.add(child);
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

int ShaderGraph::addMul(ValueType type, int matrix, int vector)
{
    auto node = Expr {};
    node.kind = ExprKind::Mul;
    node.type = type;
    node.args.add(matrix);
    node.args.add(vector);
    return add(std::move(node));
}

int ShaderGraph::addTexture()
{
    return textureSlots++;
}

int ShaderGraph::addSample(int textureSlot, int uv)
{
    auto node = Expr {};
    node.kind = ExprKind::Sample;
    node.type = ValueType::Float4;
    node.index = textureSlot;
    node.args.add(uv);
    return add(std::move(node));
}

int ShaderGraph::addThreadId()
{
    auto node = Expr {};
    node.kind = ExprKind::ThreadId;
    node.type = ValueType::UInt;
    return add(std::move(node));
}

int ShaderGraph::addStorageBuffer(BufferAccess access)
{
    storageSlots.add(access);
    return storageSlots.size() - 1;
}

int ShaderGraph::addBufferRead(int slot, int index)
{
    auto node = Expr {};
    node.kind = ExprKind::BufferRead;
    node.type = ValueType::Float;
    node.index = slot;
    node.args.add(index);
    return add(std::move(node));
}

void ShaderGraph::addStore(int slot, int index, int value)
{
    storeList.add({slot, index, value});
}
} // namespace eacp::GPU
