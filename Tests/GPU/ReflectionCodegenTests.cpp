#include "CodegenCommon.h"

#include <eacp/GPU/Codegen/ComputeKernel.h>
#include <eacp/GPU/Codegen/GeneratedShader.h>
#include <eacp/GPU/Codegen/Samplers.h>
#include <eacp/GPU/Codegen/ShaderFunction.h>
#include <eacp/GPU/Codegen/ShaderReflection.h>
#include <eacp/GPU/Codegen/ShaderStruct.h>
#include <eacp/GPU/Codegen/StorageTextures.h>
#include <eacp/GPU/Codegen/TextureFunctions.h>

#include <bit>
#include <string_view>

// ShaderReflection read from the graph: every binding kind at its slot, under
// the member name that declared it and with the stages that reach it, the
// uniform block's members at the offsets the upload writes, the vertex inputs
// and varyings with their interpolation, overrides, functions, structs, f16 and
// a kernel's workgroup - across a kernel of every binding, a render program,
// functions and overrides. GeneratedShader's bind-time answers are checked
// against the emitter's own predicates they replaced. A ShaderProgram's member
// names are ReflectionDevice/'s, in GPUTests, which links the program.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace ReflectionCodegenTests
{
namespace
{
struct Mote
{
    Float3 position;
    Float mass;

    EACP_SHADER_STRUCT(Mote, position, mass)
};

using Heights = StorageTexture2D<TextureFormat::R32Float, StorageAccess::ReadWrite>;

// A member of every binding kind, in the order the slots are taken.
struct EveryBinding final : ComputeKernel
{
    EveryBinding()
        : ComputeKernel({128})
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        auto tile = shared<Float>(64);
        write(tile, localIndex() % 64u, input[i]);
        barrier();

        auto uv = float2(toFloat(i), 0.5f);
        auto sampled = sampleLevel(image, smooth, uv, 0.f).x()
                       + sampleCompareLevel(depth, shadow, uv, 0.5f);

        write(output, i, sampled * gain + tint.x() + motes[i].mass);
        write(ints, i, toInt(uints[i]));

        auto h = toFloat(halves[i] * 0.5f + 1.f);
        write(written, i, i * 0u, float4(h, h, h, 1.f));

        auto texel = toInt(uint2(i, 0u));
        write(heights, texel, float4(tile[0u], 0.f, 0.f, 1.f));
        atomicAdd(counter, 0u, 1u);
        atomicAdd(signedCounter, 0u, 1);
    }

    Uniform<Float> gain;
    Uniform<Float3> tint;
    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UIntInputBuffer> uints;
    Uniform<IntOutputBuffer> ints;
    Uniform<HalfInputBuffer> halves;
    Uniform<AtomicBuffer> counter;
    Uniform<IntAtomicBuffer> signedCounter;
    Uniform<StructInputBuffer<Mote>> motes;
    Uniform<Texture2D> image;
    Uniform<Heights> heights;
    Uniform<WritableTexture2D> written;
    Uniform<SamplerBinding> smooth;
    Uniform<ComparisonSamplerBinding> shadow;
    Uniform<TextureDepth2D> depth;

    EACP_SHADER(gain,
                tint,
                input,
                output,
                uints,
                ints,
                halves,
                counter,
                signedCounter,
                motes,
                image,
                heights,
                written,
                smooth,
                shadow,
                depth)
};

const ShaderBinding&
    bindingAt(const ShaderReflection& reflection,
              BindingKind kind,
              int slot,
              const std::source_location& where = std::source_location::current())
{
    static const auto missing = ShaderBinding {};
    const auto* binding = reflection.findBinding(kind, slot);
    check(binding != nullptr, std::string(bindingKindName(kind)), where);
    return binding != nullptr ? *binding : missing;
}

void checkBuffer(const ShaderReflection& reflection,
                 int slot,
                 std::string_view name,
                 BufferAccess access,
                 ValueType element,
                 const std::source_location& where = std::source_location::current())
{
    const auto& binding =
        bindingAt(reflection, BindingKind::StorageBuffer, slot, where);
    check(binding.name == name, binding.name, where);
    check(binding.access == access, name, where);
    check(binding.elementType == element, name, where);
    check(binding.stages == StageVisibility {.compute = true}, name, where);
    check(reflection.findBinding(name) == &binding, name, where);
}

auto tKernel = test("Reflection/aKernelBindsEveryKindAtItsSlot") = []
{
    auto kernel = EveryBinding {};
    const auto& reflection = kernel.reflection();

    check(reflection.isCompute);
    check(reflection.bindings.size() == 1 + 8 + 6);

    const auto& block = bindingAt(reflection, BindingKind::UniformBlock, 0);
    check(block.stages == StageVisibility {.compute = true});
    check(block.name.empty());
    check(&reflection.bindings[0] == &block, "the block comes first");

    checkBuffer(reflection, 0, "input", BufferAccess::Read, ValueType::Float);
    checkBuffer(reflection, 1, "output", BufferAccess::Write, ValueType::Float);
    checkBuffer(reflection, 2, "uints", BufferAccess::Read, ValueType::UInt);
    checkBuffer(reflection, 3, "ints", BufferAccess::Write, ValueType::Int);
    checkBuffer(reflection, 4, "halves", BufferAccess::Read, ValueType::Half);
    checkBuffer(reflection, 5, "counter", BufferAccess::Atomic, ValueType::UInt);
    checkBuffer(
        reflection, 6, "signedCounter", BufferAccess::Atomic, ValueType::Int);
    checkBuffer(reflection, 7, "motes", BufferAccess::Read, ValueType::Struct);

    const auto& motes = bindingAt(reflection, BindingKind::StorageBuffer, 7);
    check(motes.structType == 0);
    check(reflection.structs[motes.structType].name == "Mote");

    const auto& image = bindingAt(reflection, BindingKind::Texture, 0);
    check(image.name == "image");
    check(image.textureKind == TextureKind::Texture2D);
    check(image.textureAccess == TextureAccess::Sample);
    check(image.isTextureSlot());

    const auto& heights = bindingAt(reflection, BindingKind::StorageTexture, 1);
    check(heights.name == "heights");
    check(heights.format == TextureFormat::R32Float);
    check(heights.textureAccess == TextureAccess::ReadWrite);
    check(heights.textureKind == TextureKind::Texture2D);

    const auto& written = bindingAt(reflection, BindingKind::StorageTexture, 2);
    check(written.name == "written");
    check(written.format == TextureFormat::RGBA32Float);
    check(written.textureAccess == TextureAccess::Write);

    check(bindingAt(reflection, BindingKind::Sampler, 3).name == "smooth");
    check(bindingAt(reflection, BindingKind::ComparisonSampler, 4).name == "shadow");

    const auto& depth = bindingAt(reflection, BindingKind::Texture, 5);
    check(depth.name == "depth");
    check(depth.textureKind == TextureKind::Depth2D);

    for (const auto& binding: reflection.bindings)
        check(binding.stages == StageVisibility {.compute = true},
              "every binding of a kernel is the compute stage's");

    check(reflection.findBinding(BindingKind::Texture, 1) == nullptr,
          "slot 1 is the storage texture's");
    check(reflection.findBinding("nothing") == nullptr);
};

auto tKernelBlock = test("Reflection/aKernelsBlockEndsInItsGridExtent") = []
{
    auto kernel = EveryBinding {};
    const auto& reflection = kernel.reflection();
    const auto& members = reflection.uniformMembers;

    check(members.size() == 3);
    check(members[0].name == "gain" && members[0].type == ValueType::Float);
    check(members[0].offset == 0 && members[0].size == 4);
    check(members[1].name == "tint" && members[1].type == ValueType::Float3);
    check(members[1].offset == 16 && members[1].size == 16);
    check(members[2].name == "count" && members[2].gridExtent);
    check(members[2].offset == 32);
    check(!members[0].gridExtent && !members[1].gridExtent);
    check(reflection.uniformBlockSize == 48);

    check(kernel.packedUniforms(7) != nullptr);
    check(
        reflection.uniformBlockSize
        == uniformBlockSize({ValueType::Float, ValueType::Float3, ValueType::UInt}));

    check(reflection.dispatchRank == DispatchRank::OneD);
    check(reflection.workgroupSize.x == 128 && reflection.workgroupSize.y == 1);
    check(reflection.threadgroupMemoryBytes == kernel.threadgroupMemoryBytes());
    check(reflection.threadgroupMemoryBytes >= 64 * 4);
    check(reflection.usesHalf, "a Half buffer is f16");
    check(!reflection.usesSampleRateShading);
    check(reflection.vertexInputs.empty() && reflection.varyings.empty());
};

struct Grid2D final : ComputeKernel
{
    Grid2D()
        : ComputeKernel({8, 4})
    {
        compile();
    }

    void define() override
    {
        auto position = threadPosition();
        write(output, position.y * 64u + position.x, toFloat(position.x));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

auto tGrid = test("Reflection/a2DKernelsExtentIsWidthAndHeight") = []
{
    auto kernel = Grid2D {};
    const auto& reflection = kernel.reflection();

    check(reflection.dispatchRank == DispatchRank::TwoD);
    check(reflection.workgroupSize.x == 8 && reflection.workgroupSize.y == 4);
    check(reflection.uniformMembers.size() == 2);
    check(reflection.uniformMembers[0].name == "width");
    check(reflection.uniformMembers[1].name == "height");
    check(reflection.uniformMembers[1].offset == 4);
    check(reflection.uniformBlockSize == 8);
    check(!reflection.usesHalf);
};

// A render program whose two stages read different things: the vertex stage a
// uniform, a texture through a sampler binding and a function; the fragment a
// uniform, a buffer, the same function and a texture; one texture neither.
struct RenderGraph
{
    RenderGraph()
    {
        auto position = builder.vertexInput<Float2>();
        auto shade = builder.vertexInput<Float3>();
        auto offset = builder.instanceInput<Float2>(2);
        auto id = builder.instanceInput<UInt>(2);

        auto wave = builder.function<Float(Float)>(
            "wave", [&](Float x) { return sin(x) * 0.5f + 0.5f; });

        auto uv = position * 0.5f + 0.5f;
        auto height = sampleLevel(heights, smooth, uv, 0.f).x();
        builder.position(
            float4(position * scale + offset, height * wave(position.x()), 1.f));

        auto carried = builder.varying(
            shade, Interpolation::linear(InterpolationSampling::Centroid));
        auto index = builder.varying(id);
        auto sampled = builder.varying(
            position, Interpolation::perspective(InterpolationSampling::Sample));

        auto colour = sample(albedo, sampled) * tint;
        auto weight = weights[index] * wave(carried.x()) + offset.y();
        builder.fragment(colour * weight);
    }

    ShaderBuilder builder;
    Float scale = builder.uniform<Float>();
    Float4 tint = builder.uniform<Float4>();
    InputBuffer weights = builder.inputBuffer();
    Texture2D heights = builder.texture();
    SamplerBinding smooth = builder.sampler();
    Texture2D albedo = builder.texture({TextureFilter::Linear});
    TextureCube unread = builder.cubeTexture();
};

auto tRender = test("Reflection/aRenderProgramsStagesReadWhatTheyReach") = []
{
    auto program = RenderGraph {};
    const auto& graph = program.builder.graph();
    const auto reflection = reflectShader(graph);

    check(!reflection.isCompute);
    check(reflection.bindings.size() == 1 + 1 + 4);

    const auto& block = bindingAt(reflection, BindingKind::UniformBlock, 0);
    check(block.stages == StageVisibility {.vertex = true, .fragment = true});
    check(block.stages.includes(ShaderStage::Vertex));
    check(!block.stages.includes(ShaderStage::Compute));

    const auto& weights = bindingAt(reflection, BindingKind::StorageBuffer, 0);
    check(weights.stages == StageVisibility {.fragment = true});
    check(weights.name.empty(), "a ShaderBuilder's own has no name");

    const auto& heights = bindingAt(reflection, BindingKind::Texture, 0);
    check(heights.stages == StageVisibility {.vertex = true});

    check(bindingAt(reflection, BindingKind::Sampler, 1).stages
          == StageVisibility {.vertex = true});

    const auto& albedo = bindingAt(reflection, BindingKind::Texture, 2);
    check(albedo.stages == StageVisibility {.fragment = true});
    check(albedo.sampling.filter == TextureFilter::Linear);

    const auto& unread = bindingAt(reflection, BindingKind::Texture, 3);
    check(unread.textureKind == TextureKind::Cube);
    check(unread.stages.isEmpty(), "declared, and read by neither stage");

    check(reflection.uniformMembers.size() == 2);
    check(reflection.uniformMembers[1].type == ValueType::Float4);
    check(reflection.uniformMembers[1].offset == 16);
    check(reflection.uniformBlockSize == 32);
    check(reflection.usesSampleRateShading, "a Sample-interpolated varying");

    const auto generated = buildShader(graph, ShaderBackend::Vulkan);
    check(generated.vertexReadsUniforms() == vertexReadsUniforms(graph));
    check(generated.fragmentReadsUniforms() == fragmentReadsUniforms(graph));

    for (auto slot = 0; slot < 32; ++slot)
        check(generated.vertexReadsTexture(slot)
                  == (((vertexTextureSlots(graph) >> slot) & 1u) != 0u),
              "the bind answer is the emitter's");

    check(std::popcount(vertexTextureSlots(graph)) == 2);
    check(generated.dispatchRank() == DispatchRank::OneD);
};

auto tInterface = test("Reflection/vertexInputsAndVaryingsInLocationOrder") = []
{
    auto program = RenderGraph {};
    const auto reflection = reflectShader(program.builder.graph());

    const auto& inputs = reflection.vertexInputs;
    check(inputs.size() == 4);
    check(inputs[0].location == 0 && inputs[0].type == ValueType::Float2);
    check(inputs[0].stepRate == StepRate::PerVertex && inputs[0].bufferIndex == 0);
    check(inputs[1].type == ValueType::Float3);
    check(inputs[2].stepRate == StepRate::PerInstance && inputs[2].bufferIndex == 2);
    check(inputs[3].type == ValueType::UInt && inputs[3].location == 3);

    const auto& varyings = reflection.varyings;
    check(varyings.size() == 4, "three declared and one input carried");
    check(varyings[0].type == ValueType::Float3);
    check(varyings[0].interpolation
          == Interpolation::linear(InterpolationSampling::Centroid));
    check(varyings[1].type == ValueType::UInt);
    check(varyings[1].interpolation == Interpolation::flat(),
          "an integer varying is flat unasked");
    check(varyings[2].interpolation.sampling == InterpolationSampling::Sample);
    check(varyings[3].attribute == 2,
          "the fragment reads offset, which no varying carries, directly");
    check(varyings[3].type == ValueType::Float2);
    check(varyings[3].interpolation == Interpolation::perspective());

    for (auto location = 0; location < varyings.size(); ++location)
        check(varyings[location].location == location);

    check(varyings[0].attribute == -1);
};

auto tFunctions = test("Reflection/functionsAndTheStagesCallingThem") = []
{
    auto program = RenderGraph {};
    const auto reflection = reflectShader(program.builder.graph());

    check(reflection.functions.size() == 1);
    const auto* wave = reflection.findFunction("wave");
    check(wave != nullptr);
    check(wave->calledFrom == StageVisibility {.vertex = true, .fragment = true});
    check(wave->returnsValue && wave->returnType == ValueType::Float);
    check(wave->parameters.size() == 1);
    check(wave->parameters[0].type == ValueType::Float);
    check(wave->isPure && !wave->calledAsStatement);
    check(!wave->writesStorage && !wave->discards);
    check(reflection.findFunction("missing") == nullptr);
};

// Functions a kernel calls - through another, with a struct, a matrix and an
// f16 - beside one it never calls, and overrides of every type.
struct FunctionKernel
{
    FunctionKernel()
    {
        auto input = builder.structInputBuffer<Mote>();
        auto output = builder.structOutputBuffer<Mote>();
        auto counts = builder.outputBuffer();
        auto id = builder.threadId();

        auto gain = builder.overrideConstant<Float>("gain", 2.f);
        auto taps = builder.overrideConstant<UInt>("taps");
        auto flip = builder.overrideConstant<Bool>("flip", true);
        auto bias = builder.overrideConstant<Int>("bias", -3);
        auto soft = builder.overrideConstant<Half>("soft", 0.25f);

        auto weigh = builder.function<Mote(Mote, Float)>(
            "weigh",
            [&](Mote mote, Float k) { return Mote {mote.position * k, mote.mass}; });

        auto turn = builder.function<Float2(Float2x2, Float2)>(
            "turn", [&](Float2x2 m, Float2 v) { return m * v; });

        auto halfOf =
            builder.function<Half(Half)>("halfOf", [&](Half h) { return h * soft; });

        auto record = builder.function<void(UInt, Float)>(
            "record",
            [&](UInt at, Float value)
            { builder.write(counts, at, value + toFloat(halfOf(toHalf(value)))); });

        auto unused = builder.function<Float(Float)>(
            "unused", [&](Float x) { return x * 3.f; });
        (void) unused;

        builder.write(output, id, weigh(input[id], gain));

        auto rotation = float2x2(float2(builder.constant(0.f), 1.f),
                                 float2(builder.constant(-1.f), 0.f));
        auto signedBias = select(flip, toFloat(bias), 0.f);
        record(id * taps, turn(rotation, float2(signedBias, 1.f)).x());
    }

    ShaderBuilder builder;
};

auto tKernelFunctions = test("Reflection/aKernelsFunctionsAndOverrides") = []
{
    auto kernel = FunctionKernel {};
    const auto reflection = reflectShader(kernel.builder.graph());

    check(reflection.functions.size() == 5);

    const auto* weigh = reflection.findFunction("weigh");
    check(weigh->calledFrom == StageVisibility {.compute = true});
    check(weigh->returnType == ValueType::Struct && weigh->returnStruct == 0);
    check(weigh->parameters.size() == 2);
    check(weigh->parameters[0].type == ValueType::Struct);
    check(weigh->parameters[0].structType == 0);
    check(weigh->parameters[1].structType == -1);
    check(weigh->calledAsStatement, "a struct result is a statement");

    const auto* turn = reflection.findFunction("turn");
    check(turn->parameters[0].type == ValueType::Float2x2);
    check(turn->returnType == ValueType::Float2);

    const auto* halfOf = reflection.findFunction("halfOf");
    check(halfOf->calledFrom.compute, "called through record");
    check(halfOf->returnType == ValueType::Half);

    const auto* record = reflection.findFunction("record");
    check(!record->returnsValue);
    check(record->writesStorage && record->calledAsStatement);

    const auto* unused = reflection.findFunction("unused");
    check(unused->calledFrom.isEmpty(), "declared, never called, never emitted");

    check(reflection.overrides.size() == 5);
    const auto* gain = reflection.findOverride("gain");
    check(gain->id == 0 && gain->type == ValueType::Float && gain->hasDefault);
    check(gain->defaultValue.asFloat() == 2.f);
    check(gain->defaultValue.type == PipelineConstantType::Float);

    const auto* taps = reflection.findOverride("taps");
    check(taps->id == 1 && !taps->hasDefault);
    check(taps->defaultValue.type == PipelineConstantType::UInt);

    check(reflection.findOverride("flip")->defaultValue.asBool());
    check(reflection.findOverride("bias")->defaultValue.asInt() == -3);

    const auto* soft = reflection.findOverride("soft");
    check(soft->type == ValueType::Half);
    check(soft->defaultValue.type == PipelineConstantType::Half);
    check(soft->defaultValue.asFloat() == 0.25f);
    check(soft->defaultValue == PipelineConstant::ofHalf(4, 0.25f));
    check(reflection.findOverride("nothing") == nullptr);

    check(reflection.usesHalf);
};

struct Inner
{
    Float2 uv;
    Half weight;

    EACP_SHADER_STRUCT(Inner, uv, weight)
};

struct Outer
{
    Float scale;
    Inner inner;
    Float3x3 basis;

    EACP_SHADER_STRUCT(Outer, scale, inner, basis)
};

struct Flagged
{
    Bool on;
    Float value;

    EACP_SHADER_STRUCT(Flagged, on, value)
};

auto tStructs = test("Reflection/structsInTheirStorageLayout") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.structInputBuffer<Outer>();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();

    auto flagged = builder.var(Flagged {builder.boolean(true), input[id].scale});
    builder.write(output, id, flagged.get().value + input[id].inner.uv.x());

    const auto reflection = reflectShader(builder.graph());

    check(reflection.structs.size() == 3, "Inner, Outer and Flagged");

    const auto* outer = &reflection.structs[1];
    check(outer->name == "Outer");
    check(outer->size == StructLayout<Outer>::size);
    check(outer->alignment == StructLayout<Outer>::alignment);
    check(outer->hostShareable);
    check(outer->members.size() == 3);
    check(outer->members[1].name == "inner");
    check(outer->members[1].type == ValueType::Struct);
    check(outer->members[1].structType == 0);
    check(outer->members[1].offset
          == StructLayout<Outer>::offsetOf<&Outer::inner>());
    check(outer->members[2].type == ValueType::Float3x3);
    check(outer->members[2].offset
          == StructLayout<Outer>::offsetOf<&Outer::basis>());

    const auto& inner = reflection.structs[0];
    check(inner.name == "Inner" && inner.members[1].type == ValueType::Half);

    check(!reflection.structs[2].hostShareable, "a Bool keeps it a value");

    const auto& buffer = bindingAt(reflection, BindingKind::StorageBuffer, 0);
    check(buffer.elementType == ValueType::Struct && buffer.structType == 1);
    check(reflection.usesHalf);
};

auto tNoUniforms = test("Reflection/aRenderProgramWithNoUniformsHasNoBlock") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    builder.position(float4(position, 0.f, 1.f));
    builder.fragment(float4(builder.varying(position), 0.f, 1.f));

    const auto generated = builder.build();
    const auto& reflection = generated.reflection;

    check(reflection.bindings.empty());
    check(reflection.uniformMembers.empty() && reflection.uniformBlockSize == 0);
    check(!generated.vertexReadsUniforms() && !generated.fragmentReadsUniforms());
    check(!generated.vertexReadsTexture(0));
    check(reflection.varyings.size() == 1);
    check(reflection.functions.empty() && reflection.overrides.empty());
    check(!reflection.usesHalf);
};

auto tSameSource = test("Reflection/reflectingChangesNoSource") = []
{
    auto program = RenderGraph {};
    const auto& graph = program.builder.graph();

    const auto before = emitMetal(graph) + emitHlsl(graph) + emitGlsl(graph);
    const auto reflection = reflectShader(graph);
    check(!reflection.bindings.empty());
    check(before == emitMetal(graph) + emitHlsl(graph) + emitGlsl(graph));

    auto kernel = EveryBinding {};
    auto unnamed = ShaderGraph {kernel.graph()};
    unnamed.nameTexture(0, "renamed");
    unnamed.nameStorageBuffer(0, "renamed too");
    check(emitMetal(unnamed) == emitMetal(kernel.graph()),
          "a name is reflection's alone");
    check(emitGlsl(unnamed) == emitGlsl(kernel.graph()));
    check(reflectShader(unnamed).findBinding("renamed")->kind
          == BindingKind::Texture);

    expectGlslCompiles(graph);
    expectGlslCompiles(emitGlsl(kernel.graph()), true);
};
} // namespace
} // namespace ReflectionCodegenTests
