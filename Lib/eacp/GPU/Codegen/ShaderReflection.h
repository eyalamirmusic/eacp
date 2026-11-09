#pragma once

#include "../Shader/PipelineConstants.h"
#include "ShaderGraph.h"

#include <string_view>

// What an EDSL shader binds and passes, read from the graph it was emitted
// from: WebGPU's reflection, and more than a compiled module can say - every
// binding with the slot a pass binds it at, the stages that reach it and the
// member that declared it; the uniform block's members at their offsets; the
// vertex inputs, the varyings and their interpolation; the overrides, the
// functions and the structs; a kernel's workgroup. One source of truth: the
// emitter answers the same questions from the same graph, and GeneratedShader's
// bind-time answers (vertexReadsUniforms, vertexReadsTexture) are read from
// here. A hand-written ShaderSource has none; see "Reflection" in the README.

namespace eacp::GPU
{
// The stages whose code reaches a binding or calls a function. Every binding a
// kernel declares is the compute stage's, read or not, as its entry point
// declares them all; a render binding is a stage's when that stage reads it,
// so one no stage reads has none.
struct StageVisibility
{
    bool includes(ShaderStage stage) const;
    bool isEmpty() const;

    bool operator==(const StageVisibility& other) const = default;

    bool vertex = false;
    bool fragment = false;
    bool compute = false;
};

enum class BindingKind : std::uint8_t
{
    UniformBlock,
    StorageBuffer,
    Texture,
    StorageTexture,
    Sampler,
    ComparisonSampler
};

std::string_view bindingKindName(BindingKind kind);

struct ShaderBinding
{
    // Buffers bind at their buffer slot, textures and samplers at a slot of
    // the texture counter they share, the uniform block at 0.
    bool isTextureSlot() const;

    BindingKind kind = BindingKind::UniformBlock;

    // What a pass binds it at: ComputePass::setInputBuffer / setOutputBuffer,
    // RenderPass::setFragmentBuffer, the texture and sampler binds, setBytes.
    int slot = 0;

    // The EACP_SHADER member that declared it; empty for a ShaderBuilder's own
    // and for the uniform block.
    std::string name;

    StageVisibility stages;

    // A StorageBuffer: Read (an input), Write (an output) or Atomic, and what
    // an element is - Float, UInt, Int, Half or Struct, with structType then
    // its index in ShaderReflection::structs.
    BufferAccess access = BufferAccess::Read;
    ValueType elementType = ValueType::Float;
    int structType = -1;

    // A Texture or a StorageTexture: its kind. A Texture's baked sampling; a
    // StorageTexture's format and access - Write, Read or ReadWrite; a
    // WritableTexture2D is a Write one of RGBA32Float.
    TextureKind textureKind = TextureKind::Texture2D;
    TextureSampling sampling;
    TextureFormat format = TextureFormat::RGBA32Float;
    TextureAccess textureAccess = TextureAccess::Sample;
};

// One member of the uniform block, at its offset in the packed block the
// upload walk fills (UniformLayout.h's MSL rules), spanning `size` bytes there.
// A kernel's block ends in the grid extent its dispatch writes - "count", or
// "width", "height" and "depth" - which are members too, marked gridExtent.
struct UniformMember
{
    std::string name;
    ValueType type = ValueType::Float;
    int offset = 0;
    int size = 0;
    bool gridExtent = false;
};

// A vertex input at its attribute location; where its bytes are is the
// GeneratedShader's vertexLayout.
struct VertexInputReflection
{
    int location = 0;
    ValueType type = ValueType::Float;
    StepRate stepRate = StepRate::PerVertex;
    int bufferIndex = 0;
};

// A varying at its location in the emitted stages: a declared one, or a vertex
// input the fragment stage reads directly, carried for it (attribute is then
// the input's location).
struct VaryingReflection
{
    int location = 0;
    ValueType type = ValueType::Float;
    Interpolation interpolation;
    int attribute = -1;
};

struct OverrideReflection
{
    int id = 0;
    std::string name;
    ValueType type = ValueType::Float;
    bool hasDefault = false;

    // The default as a pipeline constant of this id would hold it.
    PipelineConstant defaultValue;
};

struct FunctionParameterReflection
{
    ValueType type = ValueType::Float;
    int structType = -1;
};

// A function the shader declares, whether a stage calls it or not: one no
// stage calls is not emitted, and calledFrom is then empty.
struct FunctionReflection
{
    std::string name;
    Vector<FunctionParameterReflection> parameters;
    bool returnsValue = false;
    ValueType returnType = ValueType::Bool;
    int returnStruct = -1;
    StageVisibility calledFrom;
    bool calledAsStatement = false;
    bool writesStorage = false;
    bool discards = false;
    bool isPure = false;
};

// A struct the shader declares, in WGSL's storage layout: its members at their
// offsets (a nested struct's structType its index here), size - which is also
// its stride in a buffer - and alignment. Not host-shareable where it holds a
// Bool: a value only, never a buffer's element.
struct StructReflection
{
    std::string name;
    Vector<StructMember> members;
    int size = 0;
    int alignment = 1;
    bool hostShareable = true;
};

struct ShaderReflection
{
    // The binding of that kind at that slot, and the one a member of that name
    // declared; null where there is none.
    const ShaderBinding* findBinding(BindingKind kind, int slot) const;
    const ShaderBinding* findBinding(std::string_view name) const;

    const OverrideReflection* findOverride(std::string_view name) const;
    const FunctionReflection* findFunction(std::string_view name) const;

    // The uniform block, then the storage buffers in slot order, then the
    // texture slots - textures, storage textures and samplers - in slot order.
    Vector<ShaderBinding> bindings;

    Vector<UniformMember> uniformMembers;

    // The packed block's size, padded as the upload pads it; for a kernel,
    // with its grid extent.
    int uniformBlockSize = 0;

    Vector<VertexInputReflection> vertexInputs;
    Vector<VaryingReflection> varyings;
    Vector<OverrideReflection> overrides;
    Vector<FunctionReflection> functions;
    Vector<StructReflection> structs;

    bool isCompute = false;

    // A kernel's grid and the group it is dispatched in, its own or the stock
    // one for the rank, and the threadgroup memory it declares
    // (ShaderGraph::threadgroupMemoryBytes).
    DispatchRank dispatchRank = DispatchRank::OneD;
    ThreadGroupShape workgroupSize;
    int threadgroupMemoryBytes = 0;

    // What a device is asked before the shader is prepared on it:
    // Device::supportsShaderFloat16 and Device::supportsSampleRateShading.
    bool usesHalf = false;
    bool usesSampleRateShading = false;
};

ShaderReflection reflectShader(const ShaderGraph& graph);
} // namespace eacp::GPU
