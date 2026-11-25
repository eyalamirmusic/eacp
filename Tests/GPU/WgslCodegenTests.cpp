#include "CodegenCommon.h"

#include <limits>
#include <string>

// The WGSL dialect: a kernel's entry point and bindings, a render program's
// stage structs, the uniform block laid out to the bytes the CPU packs, and
// the spellings WGSL has of its own - atomics through a pointer, select, a
// switch's default, f16, the texture functions and storage textures. Checked
// as text here; every emitted program was also compiled by a browser's WebGPU
// when the dialect was written.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace WgslCodegenTests
{
namespace
{
struct Doubling
{
    Doubling()
    {
        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();
        builder.write(output, id, input[id] * 2.f);
    }

    ShaderBuilder builder;
};

struct Shaded
{
    Shaded()
    {
        auto position = builder.vertexInput<Float2>();
        auto tint = builder.uniform<Float4>();
        auto id = builder.varying(builder.vertexId());
        auto uv = builder.varying(position, Interpolation::linear());

        builder.position(float4(position, 0.f, 1.f));
        builder.fragment(tint * float4(uv, toFloat(id), 1.f));
    }

    ShaderBuilder builder;
};
} // namespace

auto tKernel = test("Wgsl/aKernelDeclaresItsEntryAndBindings") = []
{
    auto kernel = Doubling {};
    auto wgsl = emitWgsl(kernel.builder.graph());

    check(has(wgsl, "struct Uniforms\n{\n    count: u32,\n}"));
    check(has(wgsl, "@group(0) @binding(0) var<uniform> uniforms: Uniforms;"));
    check(
        has(wgsl, "@group(0) @binding(1) var<storage, read> buffer0: array<f32>;"));
    check(
        has(wgsl,
            "@group(0) @binding(2) var<storage, read_write> buffer1: array<f32>;"));
    check(has(wgsl, "@compute @workgroup_size(64, 1, 1)\nfn computeMain("));
    check(has(wgsl, "@builtin(global_invocation_id) gidIn: vec3<u32>"));
    check(has(wgsl, "@builtin(num_workgroups) gridGroups: vec3<u32>"));
    check(has(wgsl, "    let gid: u32 = gidIn.x + gidIn.y * (gridGroups.x * 64u);"));
    check(
        has(wgsl, "    if (gid >= uniforms.count)\n    {\n        return;\n    }"));
    check(has(wgsl, "buffer1[gid] = (buffer0[gid] * 2.0);"));
    check(!has(wgsl, "diagnostic"));
};

auto tRender = test("Wgsl/aRenderProgramHasBothEntryPoints") = []
{
    auto shader = Shaded {};
    auto wgsl = emitWgsl(shader.builder.graph());

    check(has(wgsl, "diagnostic(off, derivative_uniformity);"));
    check(has(wgsl, "struct VertexIn\n{\n    @location(0) a0: vec2<f32>,\n}"));
    check(has(wgsl, "    @builtin(position) position: vec4<f32>,"));
    check(has(wgsl, "    @location(0) @interpolate(flat) v0: u32,"));
    check(has(wgsl, "    @location(1) @interpolate(linear) v1: vec2<f32>,"));
    check(has(wgsl,
              "@vertex\nfn vertexMain(input: VertexIn,\n    "
              "@builtin(vertex_index) vertexId: u32) -> VertexOut"));
    check(has(wgsl, "    var output: VertexOut;"));
    check(has(wgsl, "    output.v0 = vertexId;"));
    check(has(wgsl,
              "@fragment\nfn fragmentMain(input: VertexOut) -> @location(0) "
              "vec4<f32>"));
    check(has(wgsl, "f32(input.v0)"));
};

auto tBuild = test("Wgsl/buildShaderNamesTheEntriesOfAWgslSource") = []
{
    auto shader = Shaded {};
    auto generated = buildShader(shader.builder.graph(), ShaderBackend::WebGPU);

    check(generated.source.backend == ShaderBackend::WebGPU);
    check(generated.source.vertexEntry == "vertexMain");
    check(generated.source.fragmentEntry == "fragmentMain");
    check(generated.source.source == emitWgsl(shader.builder.graph()));

    auto kernel = Doubling {};
    auto compute = buildShader(kernel.builder.graph(), ShaderBackend::WebGPU);
    check(compute.source.computeEntry == "computeMain");
    check(ShaderSource::wgsl("x").backend == ShaderBackend::WebGPU);
};

auto tBindings = test("Wgsl/bindingsRunInBlocksOfSixteen") = []
{
    check(webgpuUniformBinding == 0);
    check(webgpuBufferBinding(0) == 1 && webgpuBufferBinding(15) == 16);
    check(webgpuTextureBinding(0) == 17 && webgpuTextureBinding(15) == 32);
    check(webgpuSamplerBinding(0) == 33 && webgpuSamplerBinding(15) == 48);
};

// MSL gives a float3 sixteen bytes and a two-row matrix float4 columns; WGSL
// gives the first twelve and the second eight-byte columns, so both are
// widened to the bytes the CPU packs.
auto tUniformLayout = test("Wgsl/theUniformBlockHoldsTheBytesTheCpuPacks") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto direction = builder.uniform<Float3>();
    auto weight = builder.uniform<Float>();
    auto turn = builder.uniform<Float2x2>();
    auto offset = builder.uniform<Float2>();

    builder.position(float4(turn * position + offset, 0.f, 1.f));
    builder.fragment(float4(direction * weight, 1.f));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl,
              "struct Uniforms\n{\n"
              "    @size(16) u0: vec3<f32>,\n"
              "    @size(16) u1: f32,\n"
              "    u2: array<vec4<f32>, 2>,\n"
              "    @size(16) u3: vec2<f32>,\n"
              "}"),
          wgsl);
    check(has(wgsl, "mat2x2<f32>(uniforms.u2[0].xy, uniforms.u2[1].xy)"));
    check(uniformBlockSize({ValueType::Float3,
                            ValueType::Float,
                            ValueType::Float2x2,
                            ValueType::Float2})
          == 80);
};

auto tAtomics = test("Wgsl/atomicsActThroughAPointer") = []
{
    auto builder = ShaderBuilder {};
    auto counter = builder.atomicBuffer();
    auto output = builder.uintOutputBuffer();
    auto id = builder.threadId();

    auto ticket = builder.atomicAdd(counter, 0u, 1u);
    auto taken = builder.atomicSub(counter, 1u, 2u);
    auto swap = builder.atomicCompareExchangeWeak(counter, 2u, 0u, id);
    builder.write(output,
                  id,
                  ticket + taken + swap.oldValue + counter.load(3u)
                      + select(swap.exchanged, 1u, 0u));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl,
              "@group(0) @binding(1) var<storage, read_write> buffer0: "
              "array<atomic<u32>>;"));
    check(has(wgsl, "var v0: u32 = atomicAdd(&buffer0[0u], 1u);"));
    check(has(wgsl, "var v1: u32 = atomicSub(&buffer0[1u], 2u);"));
    check(has(wgsl, "let cx2 = atomicCompareExchangeWeak(&buffer0[2u], 0u, gid);"));
    check(has(wgsl, "var v2: u32 = cx2.old_value;"));
    check(has(wgsl, "var v3: bool = cx2.exchanged;"));
    check(has(wgsl, "atomicLoad(&buffer0[3u])"));
    check(has(wgsl, "select(0u, 1u, v3)"));
};

auto tSharedAtomics = test("Wgsl/aSharedAtomicArrayIsWorkgroupMemory") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.uintOutputBuffer();
    auto bins = builder.sharedAtomic<UInt>(4);
    auto id = builder.threadId();

    builder.write(bins, 0u, 0u);
    builder.barrier();
    auto previous = builder.atomicAdd(bins, id % 4u, 1u);
    builder.barrier();
    builder.write(output, id, previous + bins.load(id % 4u));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl, "var<workgroup> s0: array<atomic<u32>, 4>;"), wgsl);
    check(has(wgsl, "atomicStore(&s0[0u], 0u);"), wgsl);
    check(has(wgsl, "workgroupBarrier();"));
    check(has(wgsl, "atomicLoad(&s0["));
    check(!has(wgsl, "return;"));
};

// WGSL refuses a switch with no default clause, and names a clause's labels in
// one list.
auto tSwitch = test("Wgsl/everySwitchHasADefaultClause") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.uintOutputBuffer();
    auto id = builder.threadId();
    auto result = builder.var(0u);
    auto one = [&] { result = 1u; };

    builder.switchOn(id % 4u, caseOf({1, 2}, one));
    builder.write(output, id, result.get());

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl, "var v0: u32 = 0u;"));
    check(has(wgsl, "        case 1u, 2u:\n        {\n            v0 = 1u;"), wgsl);
    check(has(wgsl, "        default:\n        {\n            break;\n        }"));
};

auto tSelect = test("Wgsl/selectTakesTheFalseValueFirst") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto x = input[id];

    builder.write(output, id, select(x > 0.f, x, -x));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl, "select((-(t0)), t0, (t0 > 0.0))"), wgsl);
    check(!has(wgsl, " ? "));
};

auto tHalf = test("Wgsl/f16IsEnabledWhereTheShaderHoldsOne") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.halfInputBuffer();
    auto output = builder.halfOutputBuffer();
    auto id = builder.threadId();

    builder.write(output, id, input[id] * 0.5f);

    auto wgsl = emitWgsl(builder.graph());

    check(wgsl.starts_with("enable f16;\n"));
    check(has(wgsl, "var<storage, read> buffer0: array<f16>;"));
    check(has(wgsl, "(buffer0[gid] * 0.5h)"), wgsl);
    check(!has(emitWgsl(Doubling {}.builder.graph()), "enable f16"));
};

auto tShifts = test("Wgsl/aShiftCountIsUnsignedAtTheValuesWidth") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.uintInputBuffer();
    auto output = builder.uintOutputBuffer();
    auto id = builder.threadId();
    auto signedValue = toInt(input[id]);
    auto pair = uint2(input[id], id);

    builder.write(output,
                  id,
                  toUInt(signedValue << signedValue) + (pair >> id).x()
                      + (pair & id).y());

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl, "(t1 << u32(t1))"), wgsl);
    check(has(wgsl, ">> vec2<u32>(gid)"), wgsl);
    check(has(wgsl, "& vec2<u32>(gid)"), wgsl);
};

auto tNonFinite = test("Wgsl/anInfiniteConstantIsItsBitsAtRunTime") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();

    builder.write(
        output, id, min(input[id], std::numeric_limits<float>::infinity()));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl, "fn eacpRuntimeBits(bits: u32) -> u32"));
    check(has(wgsl, "bitcast<f32>(eacpRuntimeBits(2139095040u))"), wgsl);
};

auto tOverride = test("Wgsl/anOverrideIsKeyedByItsId") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto gain = builder.overrideConstant<Float>("gain", 1.5f);
    auto taps = builder.overrideConstant<UInt>("taps");

    builder.write(output, id, gain * toFloat(taps));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl, "@id(0) override override_gain: f32 = 1.5;"), wgsl);
    check(has(wgsl, "@id(1) override override_taps: u32 = 0u;"));
};

auto tTextures = test("Wgsl/texturesAndTheirSamplersSitAtTheirSlots") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto shadow = builder.depthTexture({.compare = TextureCompare::LessEqual});
    auto uv = builder.varying(position);

    builder.position(float4(position, 0.f, 1.f));
    builder.fragment(sample(image, uv)
                         * sampleCompare(shadow, uv, builder.constant(0.5f))
                     + sampleLevel(image, uv, 2.f));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl, "@group(0) @binding(17) var texture0: texture_2d<f32>;"), wgsl);
    check(has(wgsl, "@group(0) @binding(33) var sampler0: sampler;"));
    check(has(wgsl, "@group(0) @binding(18) var texture1: texture_depth_2d;"));
    check(has(wgsl, "@group(0) @binding(34) var sampler1: sampler_comparison;"));
    check(has(wgsl, "textureSample(texture0, sampler0, input.v0)"));
    check(has(wgsl, "textureSampleCompareLevel(texture1, sampler1, input.v0, 0.5)"));
    check(has(wgsl, "textureSampleLevel(texture0, sampler0, input.v0, 2.0)"));
};

auto tStorage = test("Wgsl/aStorageTextureNamesItsFormat") = []
{
    using Heights =
        StorageTexture2D<TextureFormat::R32Float, StorageAccess::ReadWrite>;
    using Ids = StorageTexture2D<TextureFormat::R32Uint, StorageAccess::Read>;
    using Colours = StorageTexture2DArray<TextureFormat::RGBA8Unorm>;

    auto builder = ShaderBuilder {};
    auto heights = builder.storageTexture<Heights>();
    auto ids = builder.storageTexture<Ids>();
    auto colours = builder.storageTexture<Colours>();
    auto p = toInt(builder.threadId2());
    auto h = load(heights, p);

    builder.write(heights, p, h * 0.5f);
    builder.write(colours, p, 1, float4(toFloat(load(ids, p).x()), 0.f, 0.f, 1.f));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl,
              "@group(0) @binding(17) var texture0: "
              "texture_storage_2d<r32float, read_write>;"),
          wgsl);
    check(has(wgsl, "@group(0) @binding(18) var texture1: texture_2d<u32>;"));
    check(has(wgsl,
              "@group(0) @binding(19) var texture2: "
              "texture_storage_2d_array<rgba8unorm, write>;"));
    check(has(wgsl, "textureLoad(texture0, vec2<u32>("));
    check(has(wgsl, "textureLoad(texture1, "));
    check(has(wgsl, "textureStore(texture2, vec2<u32>("));
};

auto tMatrixDivide = test("Wgsl/aMatrixOverAScalarDividesEachColumn") = []
{
    auto builder = ShaderBuilder {};
    auto position = builder.vertexInput<Float2>();
    auto turn = builder.uniform<Float2x2>();
    auto scale = builder.uniform<Float>();

    builder.position(float4((turn / scale) * position, 0.f, 1.f));
    builder.fragment(float4(scale, 0.f, 0.f, 1.f));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl,
              "fn eacpMatrixDivide_float2x2(m: mat2x2<f32>, s: f32) -> "
              "mat2x2<f32>\n{\n    return mat2x2<f32>(m[0] / s, m[1] / s);\n}"),
          wgsl);
    check(has(wgsl, "eacpMatrixDivide_float2x2(mat2x2<f32>("));
};

auto tHelpers = test("Wgsl/aHelperWgslLacksIsDefinedPerArgumentType") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto x = input[id];

    builder.write(output,
                  id,
                  erf(x) + erf(input.read4(id)).w() + log10(x) + rsqrt(x)
                      + toFloat(countOneBits(asUInt(x))));

    auto wgsl = emitWgsl(builder.graph());

    check(has(wgsl, "fn eacpErf_float(x: f32) -> f32"), wgsl);
    check(has(wgsl, "fn eacpErf_float4(x: vec4<f32>) -> vec4<f32>"));
    check(has(wgsl, "fn eacpLog10_float(x: f32) -> f32"));
    check(has(wgsl, "inverseSqrt("));
    check(has(wgsl, "countOneBits(bitcast<u32>("));
    check(!has(wgsl, "fn eacpErf_float2"));
};
} // namespace WgslCodegenTests
