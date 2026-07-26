#include "ShaderGraph.h"

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
    inputRates.add(StepRate::PerVertex);
    inputSlots.add(0);
    return add(std::move(node));
}

int ShaderGraph::addInstanceInput(ValueType type)
{
    return addInstanceInput(type, 1);
}

int ShaderGraph::addInstanceInput(ValueType type, int bufferIndex)
{
    auto node = Expr {};
    node.kind = ExprKind::Input;
    node.type = type;
    node.index = inputTypes.size();
    inputTypes.add(type);
    inputRates.add(StepRate::PerInstance);
    inputSlots.add(bufferIndex);
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

int ShaderGraph::addUIntConstant(unsigned value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::UInt;
    node.index = (int) value;
    return add(std::move(node));
}

int ShaderGraph::addIntConstant(int value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::Int;
    node.index = value;
    return add(std::move(node));
}

int ShaderGraph::addBoolConstant(bool value)
{
    auto node = Expr {};
    node.kind = ExprKind::Constant;
    node.type = ValueType::Bool;
    node.index = value ? 1 : 0;
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

int ShaderGraph::addBinary(ValueType type, std::string op, int lhs, int rhs)
{
    auto node = Expr {};
    node.kind = ExprKind::Binary;
    node.type = type;
    node.text = std::move(op);
    node.args.add(lhs);
    node.args.add(rhs);
    return add(std::move(node));
}

int ShaderGraph::addCompare(std::string op, int lhs, int rhs)
{
    return addCompare(ValueType::Bool, std::move(op), lhs, rhs);
}

int ShaderGraph::addCompare(ValueType type, std::string op, int lhs, int rhs)
{
    auto node = Expr {};
    node.kind = ExprKind::Compare;
    node.type = type;
    node.text = std::move(op);
    node.args.add(lhs);
    node.args.add(rhs);
    return add(std::move(node));
}

int ShaderGraph::addSelect(ValueType type,
                           int condition,
                           int whenTrue,
                           int whenFalse)
{
    auto node = Expr {};
    node.kind = ExprKind::Select;
    node.type = type;
    node.args.add(condition);
    node.args.add(whenTrue);
    node.args.add(whenFalse);
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

int ShaderGraph::addTexture(TextureSampling sampling)
{
    textureSamplings.add(sampling);
    return textureSamplings.size() - 1;
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

int ShaderGraph::addSample(int textureSlot, int uv, int level)
{
    auto node = Expr {};
    node.kind = ExprKind::Sample;
    node.type = ValueType::Float4;
    node.index = textureSlot;
    node.args.add(uv);
    node.args.add(level);
    return add(std::move(node));
}

int ShaderGraph::addFetch(int textureSlot, int coordinates)
{
    auto node = Expr {};
    node.kind = ExprKind::Fetch;
    node.type = ValueType::Float4;
    node.index = textureSlot;
    node.args.add(coordinates);
    return add(std::move(node));
}

int ShaderGraph::addArray(ValueType elementType, Vector<int> elements)
{
    arrayConstants.add({elementType, std::move(elements)});
    return arrayConstants.size() - 1;
}

int ShaderGraph::addArrayRead(int slot, int index)
{
    auto node = Expr {};
    node.kind = ExprKind::ArrayRead;
    node.type = arrayConstants[slot].elementType;
    node.index = slot;
    node.args.add(index);
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

int ShaderGraph::addStatement(Statement newStatement)
{
    statementList.add(newStatement);
    auto index = statementList.size() - 1;
    blocks[openBlocks.back()].statements.add(index);
    return index;
}

int ShaderGraph::addVariable(ValueType type, int initialValue)
{
    auto slot = variableTypes.size();
    variableTypes.add(type);

    auto declaration = Statement {StatementKind::Declare};
    declaration.slot = slot;
    declaration.value = initialValue;
    addStatement(declaration);

    return slot;
}

int ShaderGraph::addVarRead(int slot)
{
    auto node = Expr {};
    node.kind = ExprKind::VarRead;
    node.type = variableTypes[slot];
    node.index = slot;
    return add(std::move(node));
}

void ShaderGraph::assign(int slot, int value)
{
    auto assignment = Statement {StatementKind::Assign};
    assignment.slot = slot;
    assignment.value = value;
    addStatement(assignment);
}

int ShaderGraph::pushBlock()
{
    blocks.add(Block {});
    auto index = blocks.size() - 1;
    openBlocks.add(index);
    return index;
}

void ShaderGraph::popBlock()
{
    openBlocks.pop_back();
}

void ShaderGraph::addIf(int condition, int body, int elseBody)
{
    auto branch = Statement {StatementKind::If};
    branch.value = condition;
    branch.body = body;
    branch.elseBody = elseBody;
    addStatement(branch);
}

void ShaderGraph::addLoop(int condition, int body)
{
    auto loop = Statement {StatementKind::Loop};
    loop.value = condition;
    loop.body = body;
    addStatement(loop);
}

void ShaderGraph::addBreak()
{
    addStatement(Statement {StatementKind::Break});
}

void ShaderGraph::addContinue()
{
    addStatement(Statement {StatementKind::Continue});
}
} // namespace eacp::GPU
