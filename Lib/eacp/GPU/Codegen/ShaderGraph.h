#pragma once

#include "ShaderTypes.h"

#include <eacp/Core/Utils/Containers.h>

#include <string>

namespace eacp::GPU
{
enum class ExprKind
{
    Input, // vertex attribute; index = attribute slot
    Varying, // fragment-stage read of a varying; index = varying slot
    Constant, // scalar float literal; value
    Construct, // floatN(args...); args = child nodes
    Swizzle, // child.<components>; args[0] = child
    Binary // (lhs op rhs); args = {lhs, rhs}
};

// One node in the shader expression tree. Plain data referenced by integer id so
// value handles stay trivially copyable and the graph owns every node.
struct Expr
{
    ExprKind kind = ExprKind::Constant;
    ValueType type = ValueType::Float;
    int index = 0; // Input / Varying slot
    float value = 0.0f; // Constant
    char op = '+'; // Binary
    std::string swizzle; // Swizzle components, e.g. "xy"
    Vector<int> args; // child node ids
};

// Backend-agnostic shader IR: an expression-node store plus the shader's I/O
// (ordered vertex inputs, ordered varyings, the clip-space position expression
// and the fragment-output expression). The same node list drives both the
// emitted source and the vertex layout, so a shader and its layout cannot drift.
// Built by ShaderBuilder, read by the emitters; never uses runtime reflection.
class ShaderGraph
{
public:
    struct VaryingSlot
    {
        ValueType type = ValueType::Float;
        int sourceNode = -1; // vertex-stage expression feeding this varying
    };

    int addInput(ValueType type);
    int addVarying(ValueType type, int sourceNode);
    int addConstant(float value);
    int addConstruct(ValueType type, Vector<int> args);
    int addSwizzle(ValueType type, int child, std::string components);
    int addBinary(ValueType type, char op, int lhs, int rhs);

    void setPosition(int node) { positionNode = node; }
    void setFragment(int node) { fragmentNode = node; }

    const Expr& expr(int node) const { return nodes[node]; }
    const Vector<ValueType>& inputs() const { return inputTypes; }
    const Vector<VaryingSlot>& varyings() const { return varyingSlots; }
    int position() const { return positionNode; }
    int fragment() const { return fragmentNode; }

private:
    int add(Expr node);

    Vector<Expr> nodes;
    Vector<ValueType> inputTypes;
    Vector<VaryingSlot> varyingSlots;
    int positionNode = -1;
    int fragmentNode = -1;
};
} // namespace eacp::GPU
