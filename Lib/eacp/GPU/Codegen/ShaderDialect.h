#pragma once

#include "ShaderGraph.h"

#include <string>

// The spellings the three dialects disagree about, one function per question.
// MSL and HLSL spell most of an expression identically; GLSL differs in a
// countable list, each with one arm here.

namespace eacp::GPU::ShaderEmission
{
enum class Backend
{
    Metal,
    DirectX,
    Vulkan
};

const char* typeName(Backend backend, ValueType type);
std::string floatLiteral(float value);

bool isFlatVarying(ValueType type);
std::string attributeSemantic(Backend backend, int index);
std::string varyingSemantic(Backend backend, int index, ValueType type);
std::string positionSemantic(Backend backend);
std::string flatQualifier(Backend backend, ValueType type);
std::string locationLayout(int index);
std::string attributeName(Backend backend, int slot);
std::string varyingName(Backend backend, int index);

std::string callName(Backend backend, const std::string& name);
bool isGenTypeCall(const std::string& name);
const char* glslComparison(const std::string& op);

std::string hlslSamplerName(const TextureSampling& sampling);
const char* componentSuffix(int component);
const char* vectorComponentSuffix(int component);
const char* metalPackedVectorType(ValueType type);

std::string indexReference(const char* name, DispatchRank rank, int component);
const char* gridExtentName(int component);
int gridExtentCount(DispatchRank rank);
const char* indexTypeName(DispatchRank rank);
const char* glslIndexTypeName(DispatchRank rank);
const char* indexSwizzle(DispatchRank rank);

std::string bracketed(const std::string& expression);
} // namespace eacp::GPU::ShaderEmission
