#pragma once

#include "ShaderDialect.h"

namespace eacp::GPU::ShaderEmission
{
struct ExprPrinter
{
    std::string printReference(int node) const;
    std::string print(int node) const;
    std::string printConverted(int node, ValueType type) const;

    // A storage texture's texel address in the dialect's own integer vector,
    // the layer beside it (Metal) or inside it (HLSL, GLSL).
    std::string printStorageAddress(int slot, int coordinates, int layer) const;

    // One element of storage slot `slot` at an already printed index.
    std::string printBufferElementRead(int slot, const std::string& index) const;

    // What a local naming node `node`, and variable `slot`, is declared as: a
    // struct's name where it holds one.
    std::string printTypeName(int node) const;
    std::string printVariableTypeName(int slot) const;

    const ShaderGraph& graph;
    Backend backend;
    const Vector<int>& locals; // node id -> local index, -1 = inline
    Vector<int> attributeVaryings; // attribute slot -> varying, -1 = read direct

private:
    int findCarryingVarying(int slot) const;
    std::string printRoundedToHalf(const Expr& expr, const std::string& text) const;
    ValueType getBroadcastType(const Expr& call) const;
    std::string printWidened(int node, ValueType wide) const;
    bool isGlslShift(const std::string& op) const;
    bool isGlslSignedRemainder(const std::string& op, ValueType type) const;
    std::string printArguments(const Expr& expr, ValueType wide) const;

    std::string printMetalLayer(int node) const;
    std::string printFloatLayer(int layer) const;
    std::string printLayeredCoordinate(int uv, int layer) const;
    std::string printLayerSample(const Expr& expr,
                                 const std::string& name,
                                 const std::string& sampler) const;
    std::string printCompareSample(const Expr& expr,
                                   const std::string& name,
                                   const std::string& sampler) const;

    std::string printInput(const Expr& expr) const;
    std::string printVarying(const Expr& expr) const;
    std::string printUniform(const Expr& expr) const;
    std::string printSwizzle(const Expr& expr) const;
    std::string printVarRead(const Expr& expr) const;
    std::string printThreadId(const Expr& expr) const;
    std::string printLocalId(const Expr& expr) const;
    std::string printGroupId(const Expr& expr) const;
    std::string printBuiltInNode(const Expr& expr) const;
    std::string printConstant(const Expr& expr) const;
    std::string printConstruct(const Expr& expr) const;
    std::string printCall(const Expr& expr) const;
    std::string printUnary(const Expr& expr) const;
    std::string printBinary(const Expr& expr) const;
    std::string printCompare(const Expr& expr) const;
    std::string printSelect(const Expr& expr) const;
    std::string printMul(const Expr& expr) const;
    std::string printMatrixColumn(const Expr& expr) const;
    std::string printOverride(const Expr& expr) const;
    std::string printFunctionCall(const Expr& expr) const;
    std::string printSample(const Expr& expr) const;
    std::string printPlainSample(const Expr& expr,
                                 const std::string& name,
                                 const std::string& sampler) const;
    std::string printFetch(const Expr& expr) const;
    std::string printTextureFunction(const Expr& expr) const;
    std::string printMetalTextureFunction(const Expr& expr) const;
    std::string printHlslTextureFunction(const Expr& expr) const;
    std::string printGlslTextureFunction(const Expr& expr) const;
    std::string printGlslTextureCompare(const Expr& expr) const;
    std::string printTextureLocation(const Expr& expr) const;
    std::string printTexelLoad(const Expr& expr) const;
    std::string printStorageLoad(const Expr& expr) const;
    std::string printTextureQuery(const Expr& expr) const;
    std::string printStorageQuery(const Expr& expr) const;
    std::string printBufferRead(const Expr& expr) const;
    std::string printStructBufferRead(const Expr& expr) const;
    std::string printStructValue(const Expr& expr) const;
    std::string printBufferVectorRead(const Expr& expr) const;
    std::string printAtomicLoad(const Expr& expr) const;
    std::string printArrayRead(const Expr& expr) const;
    std::string printGridExtent(const Expr& expr) const;
    std::string printSharedRead(const Expr& expr) const;
    std::string printSimdGroupIndex(const Expr& expr) const;
    std::string printBuiltIn(ShaderBuiltIn which) const;
    std::string printFragmentCoordinate() const;
    std::string printGroupCount() const;
    std::string printMetalGroupCount() const;
};
} // namespace eacp::GPU::ShaderEmission
