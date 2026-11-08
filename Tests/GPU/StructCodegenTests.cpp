#include "CodegenCommon.h"

#include <initializer_list>
#include <source_location>
#include <string>

// WGSL's structs as the three dialects spell them, and the storage layout all
// three are made to agree with. StructLayout is WGSL's: each member at the
// next offset its alignment allows, a three-vector aligned to sixteen bytes
// (eight for f16) and twelve (six) long, a matrix its columns at the column
// vector's alignment, a struct aligned to its widest member and sized to a
// multiple of that. GLSL declares the struct as it is under std430, whose
// rules those are; MSL lays it out by hand, a three-vector packed and a uchar
// array in every gap; HLSL keeps a plain value struct and reaches the buffer
// as bytes at the offsets. A struct is declared once, only where a shader
// uses one. The GLSL is compiled wherever eacp-spirv is built; the numbers are
// pinned by CpuComputeTests' Struct/ and on a device by StructDevice/.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace StructCodegenTests
{
namespace
{
struct Particle
{
    Float3 position;
    Float mass;
    Float2 velocity;
    UInt flags;

    EACP_SHADER_STRUCT(Particle, position, mass, velocity, flags)
};

struct VectorThenScalar
{
    Float3 v;
    Float s;

    EACP_SHADER_STRUCT(VectorThenScalar, v, s)
};

struct ScalarThenVector
{
    Float s;
    Float3 v;

    EACP_SHADER_STRUCT(ScalarThenVector, s, v)
};

struct Matrices
{
    Float3x3 m33;
    Float2x3 m23;
    Float3x2 m32;
    Float s;

    EACP_SHADER_STRUCT(Matrices, m33, m23, m32, s)
};

struct HalfMembers
{
    Half a;
    Half3 b;
    Half c;
    Half2 d;
    Float e;
    Half3x3 m;

    EACP_SHADER_STRUCT(HalfMembers, a, b, c, d, e, m)
};

struct Inner
{
    Float2 xy;
    Half h;

    EACP_SHADER_STRUCT(Inner, xy, h)
};

struct Outer
{
    Float a;
    Inner inner;
    Float3 v;
    Half2x2 hm;
    Float2x3 m;
    Int3 cell;

    EACP_SHADER_STRUCT(Outer, a, inner, v, hm, m, cell)
};

struct Halves
{
    Half value;

    EACP_SHADER_STRUCT(Halves, value)
};

struct Flagged
{
    Bool on;
    Float weight;

    EACP_SHADER_STRUCT(Flagged, on, weight)
};

static_assert(StructLayout<VectorThenScalar>::offsetOf<&VectorThenScalar::s>()
              == 12);
static_assert(StructLayout<VectorThenScalar>::size == 16);
static_assert(StructLayout<ScalarThenVector>::offsetOf<&ScalarThenVector::v>()
              == 16);
static_assert(StructLayout<ScalarThenVector>::size == 32);
static_assert(StructLayout<Matrices>::offsetOf<&Matrices::m23>() == 48);
static_assert(StructLayout<Matrices>::offsetOf<&Matrices::m32>() == 80);
static_assert(StructLayout<Matrices>::offsetOf<&Matrices::s>() == 104);
static_assert(StructLayout<Matrices>::size == 112);
static_assert(StructLayout<HalfMembers>::offsetOf<&HalfMembers::b>() == 8);
static_assert(StructLayout<HalfMembers>::offsetOf<&HalfMembers::c>() == 14);
static_assert(StructLayout<HalfMembers>::offsetOf<&HalfMembers::d>() == 16);
static_assert(StructLayout<HalfMembers>::offsetOf<&HalfMembers::e>() == 20);
static_assert(StructLayout<HalfMembers>::offsetOf<&HalfMembers::m>() == 24);
static_assert(StructLayout<HalfMembers>::size == 48);
static_assert(StructLayout<Inner>::size == 16
              && StructLayout<Inner>::alignment == 8);
static_assert(StructLayout<Outer>::offsetOf<&Outer::inner>() == 8);
static_assert(StructLayout<Outer>::offsetOf<&Outer::v>() == 32);
static_assert(StructLayout<Outer>::offsetOf<&Outer::hm>() == 44);
static_assert(StructLayout<Outer>::offsetOf<&Outer::m>() == 64);
static_assert(StructLayout<Outer>::offsetOf<&Outer::cell>() == 96);
static_assert(StructLayout<Outer>::stride == 112);
static_assert(StructLayout<Outer>::fieldCount == 8);
static_assert(StructLayout<Halves>::stride == 2);
static_assert(!StructLayout<Flagged>::hostShareable);

struct Sources
{
    explicit Sources(const ShaderGraph& graph)
        : metal(emitMetal(graph))
        , hlsl(emitHlsl(graph))
        , glsl(emitGlsl(graph))
    {
    }

    std::string metal;
    std::string hlsl;
    std::string glsl;
};

void checkAll(const std::string& source,
              std::initializer_list<const char*> expected,
              const std::source_location& location = std::source_location::current())
{
    for (const auto* text: expected)
        check(contains(source, text), std::string(text) + "\n" + source, location);
}

void checkNone(
    const std::string& source,
    std::initializer_list<const char*> refused,
    const std::source_location& location = std::source_location::current())
{
    for (const auto* text: refused)
        check(!contains(source, text), std::string(text) + "\n" + source, location);
}

// A whole element written from the one read, then one member of it.
struct SmallGraph
{
    SmallGraph()
    {
        auto input = builder.structInputBuffer<Particle>();
        auto output = builder.structOutputBuffer<Particle>();
        auto id = builder.threadId();

        auto p = input[id];
        auto moved = Particle {p.position + float3(p.velocity, 0.f),
                               p.mass * 0.5f,
                               p.velocity,
                               p.flags | 1u};

        builder.write(output, id, moved);
        builder.write(output, id, &Particle::mass, output[id].mass + 1.f);
    }

    ShaderBuilder builder;
};

constexpr auto smallMetal = std::string_view {R"golden(#include <metal_stdlib>
using namespace metal;

struct Particle
{
    packed_float3 position;
    float mass;
    float2 velocity;
    uint flags;
    uchar eacpPad0[4];
};

Particle eacpMakeParticle(float3 m0, float m1, float2 m2, uint m3)
{
    Particle value = {};
    value.position = m0;
    value.mass = m1;
    value.velocity = m2;
    value.flags = m3;
    return value;
}

struct Uniforms
{
    uint count;
};

kernel void computeMain(device const Particle* buffer0 [[buffer(0)]],
    device Particle* buffer1 [[buffer(1)]],
    constant Uniforms& uniforms [[buffer(16)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= uniforms.count)
        return;
    float2 t0 = buffer0[gid].velocity;
    buffer1[gid] = eacpMakeParticle((float3(buffer0[gid].position) + float3(t0, 0.0)), (buffer0[gid].mass * 0.5), t0, (buffer0[gid].flags | 1u));
    buffer1[gid].mass = (buffer1[gid].mass + 1.0);
}
)golden"};

constexpr auto smallHlsl = std::string_view {R"golden(struct Particle
{
    float3 position;
    float mass;
    float2 velocity;
    uint flags;
};

Particle eacpMakeParticle(float3 m0, float m1, float2 m2, uint m3)
{
    Particle value;
    value.position = m0;
    value.mass = m1;
    value.velocity = m2;
    value.flags = m3;
    return value;
}

struct Uniforms
{
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

ByteAddressBuffer buffer0 : register(t0);
RWByteAddressBuffer buffer1 : register(u1);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
    if (gid >= uniforms.count)
        return;
    float2 t0 = asfloat(buffer0.Load2((gid) * 32u + 16u));
    Particle t1 = eacpMakeParticle((asfloat(buffer0.Load3((gid) * 32u)) + float3(t0, 0.0)), (asfloat(buffer0.Load((gid) * 32u + 12u)) * 0.5), t0, (buffer0.Load((gid) * 32u + 24u) | 1u));
    buffer1.Store3((gid) * 32u, asuint(t1.position));
    buffer1.Store((gid) * 32u + 12u, asuint(t1.mass));
    buffer1.Store2((gid) * 32u + 16u, asuint(t1.velocity));
    buffer1.Store((gid) * 32u + 24u, asuint(t1.flags));
    buffer1.Store((gid) * 32u + 12u, asuint((asfloat(buffer1.Load((gid) * 32u + 12u)) + 1.0)));
}
)golden"};

constexpr auto smallGlsl = std::string_view {R"golden(#version 450

struct Particle
{
    vec3 position;
    float mass;
    vec2 velocity;
    uint flags;
};

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint count;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    Particle buffer0[];
};
layout(std430, set = 0, binding = 1) buffer Buffer1
{
    Particle buffer1[];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    vec2 t0 = buffer0[gid].velocity;
    buffer1[gid] = Particle((buffer0[gid].position + vec3(t0, 0.0)), (buffer0[gid].mass * 0.5), t0, (buffer0[gid].flags | 1u));
    buffer1[gid].mass = (buffer1[gid].mass + 1.0);
}
)golden"};

// A nested struct, a half matrix, a three-vector after a nested struct and an
// integer one at the end; written whole, a nested member at a time and as a
// half matrix; and a buffer of a struct of halves alone.
struct NestedGraph
{
    NestedGraph()
    {
        auto input = builder.structInputBuffer<Outer>();
        auto output = builder.structOutputBuffer<Outer>();
        auto halves = builder.structOutputBuffer<Halves>();
        auto id = builder.threadId();

        auto o = input[id];
        auto inner = Inner {o.inner.xy * 2.f, o.inner.h + 1.f};

        builder.write(output, id, Outer {o.a, inner, o.v, o.hm, o.m, o.cell + 1});
        builder.write(output, id + 1u, &Outer::inner, inner);
        builder.write(output, id + 2u, &Outer::hm, o.hm * o.inner.h);
        builder.write(halves, id, Halves {o.inner.h});
    }

    ShaderBuilder builder;
};

struct VarsGraph
{
    VarsGraph()
    {
        auto input = builder.structInputBuffer<Particle>();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();

        auto p = builder.var(input[id]);
        p.set(&Particle::mass, p.get().mass * 2.f);

        auto list = builder.privateArray<Particle, 4>();
        list.set(1, p.get());
        list.set(toInt(id), &Particle::velocity, p.get().velocity.yx());

        auto flag = builder.var(Flagged {id < 3u, input[id].mass});
        flag.set(&Flagged::weight, flag.get().weight + 1.f);

        auto chosen = list[toInt(id) & 3];
        builder.write(
            output, id, select(flag.get().on, chosen.mass, chosen.velocity.y()));
    }

    ShaderBuilder builder;
};

// Particle in two buffers and a variable, Inner only inside Outer's variable.
struct ManyUsesGraph
{
    ManyUsesGraph()
    {
        auto first = builder.structInputBuffer<Particle>();
        auto second = builder.structOutputBuffer<Particle>();
        auto id = builder.threadId();

        auto held = builder.var(first[id]);
        auto outer = builder.var(builder.structZero<Outer>());
        outer.set(&Outer::a, held.get().mass);

        builder.write(second, id, held.get());
        builder.write(second, id + 1u, &Particle::mass, outer.get().inner.xy.x());
    }

    ShaderBuilder builder;
};

struct FragmentGraph
{
    FragmentGraph()
    {
        auto position = builder.vertexInput<Float2>();
        auto particles = builder.structInputBuffer<Particle>();
        builder.position(float4(position, 0.f, 1.f));

        auto p = particles[0u];
        builder.fragment(float4(p.position, p.mass));
    }

    ShaderBuilder builder;
};

struct PlainGraph
{
    PlainGraph()
    {
        auto input = builder.inputBuffer();
        auto output = builder.outputBuffer();
        auto id = builder.threadId();
        builder.write(output, id, input[id] * 2.f);
    }

    ShaderBuilder builder;
};
} // namespace

auto tLayout = test("StructCodegen/theLayoutIsWgslStorageLayout") = []
{
    auto builder = ShaderBuilder {};
    builder.structOutputBuffer<Outer>();
    const auto& graph = builder.graph();

    check(graph.structCount() == 2, "the nested struct is declared first");
    check(graph.structType(0).name == "Inner"
          && graph.structType(1).name == "Outer");

    const auto& outer = graph.structType(1);
    check(outer.size == 112 && outer.alignment == 16 && outer.hostShareable);
    check(outer.fields.size() == 8);
    check(outer.fields[2].path == "inner.xy" && outer.fields[2].offset == 8);
    check(outer.fields[3].path == "inner.h" && outer.fields[3].offset == 16);
    check(outer.fields[1].type == ValueType::Struct
          && outer.fields[1].firstComponent == 1
          && outer.fields[1].componentCount == 3);

    check(outer.components.size() == 1 + 3 + 3 + 4 + 6 + 3);
    check(outer.components[3].offset == 16 && outer.components[3].half);
    check(outer.components[7].offset == 44 && outer.components[8].offset == 46,
          "a half matrix's column is packed halves");
    check(outer.components[11].offset == 64 && outer.components[14].offset == 80,
          "a float2x3's columns sit sixteen bytes apart");

    check(StructLayout<Particle>::members[3].offset == 24);
    check(StructLayout<Particle>::size == 32);
};

auto tSmall = test("StructCodegen/aSmallKernelMatchesTheGoldens") = []
{
    auto graph = SmallGraph {};
    auto sources = Sources {graph.builder.graph()};

    check(sources.metal == smallMetal, sources.metal);
    check(sources.hlsl == smallHlsl, sources.hlsl);
    check(sources.glsl == smallGlsl, sources.glsl);
    expectGlslCompiles(sources.glsl, true);
};

auto tDialects = test("StructCodegen/eachDialectLaysTheStructOutItsOwnWay") = []
{
    auto graph = NestedGraph {};
    auto sources = Sources {graph.builder.graph()};

    checkAll(sources.metal,
             {"struct Inner\n{\n    float2 xy;\n    half h;\n    uchar "
              "eacpPad0[6];\n};",
              "struct Outer\n{\n    float a;\n    uchar eacpPad0[4];\n    Inner "
              "inner;\n    uchar eacpPad1[8];\n    packed_float3 v;\n    half2x2 "
              "hm;\n    uchar eacpPad2[12];\n    float2x3 m;\n    packed_int3 "
              "cell;\n    uchar eacpPad3[4];\n};",
              "Outer eacpMakeOuter(float m0, Inner m1, float3 m2, half2x2 m3, "
              "float2x3 m4, int3 m5)",
              "device const Outer* buffer0 [[buffer(0)]]",
              "device Halves* buffer2 [[buffer(2)]]",
              "float3(buffer0[gid].v)",
              "int3(buffer0[gid].cell)",
              "buffer1[(gid + 1u)].inner = eacpMakeInner(t0, t2);",
              "buffer2[gid] = eacpMakeHalves(t1);"});

    checkAll(sources.hlsl,
             {"struct Outer\n{\n    float a;\n    Inner inner;\n    float3 v;\n    "
              "float2x2 hm;\n    float3x2 m;\n    int3 cell;\n};",
              "ByteAddressBuffer buffer0 : register(t0);",
              "RWByteAddressBuffer buffer2 : register(u2);",
              "f16tof32(buffer0.Load((gid) * 112u + 16u))",
              "f16tof32(buffer0.Load((gid) * 112u + 44u) >> 16u)",
              "transpose(float2x3(asfloat(buffer0.Load3((gid) * 112u + 64u)), "
              "asfloat(buffer0.Load3((gid) * 112u + 80u))))",
              "asint(buffer0.Load3((gid) * 112u + 96u))",
              "buffer1.Store3((gid) * 112u + 64u, asuint(transpose(t4.m)[0]));",
              "buffer1.InterlockedAnd((gid) * 112u + 44u, ~(0xffffu << 16u));",
              "buffer1.InterlockedOr((gid) * 112u + 44u, "
              "f32tof16(transpose(t4.hm)[0].y) << 16u);",
              "buffer1.Store2((t5) * 112u + 8u, asuint(t6.xy));",
              "buffer2.InterlockedAnd(((gid) * 2u) & ~3u, ~(0xffffu << ((((gid) * "
              "2u) & 2u) << 3u)));"});
    checkNone(sources.hlsl, {"StructuredBuffer", "uchar"});

    checkAll(sources.glsl,
             {"#extension GL_EXT_shader_explicit_arithmetic_types_float16 : "
              "require\n#extension GL_EXT_shader_16bit_storage : require",
              "struct Inner\n{\n    vec2 xy;\n    float16_t h;\n};",
              "struct Outer\n{\n    float a;\n    Inner inner;\n    vec3 v;\n    "
              "f16mat2 hm;\n    mat2x3 m;\n    ivec3 cell;\n};",
              "Outer buffer0[];",
              "Halves buffer2[];",
              "buffer1[(gid + 2u)].hm = (t3 * t1);"});
    checkNone(sources.glsl, {"eacpMake", "eacpPad"});

    check(graph.builder.graph().usesHalf()
          && graph.builder.graph().usesHalfStorage());
    expectGlslCompiles(sources.glsl, true);
};

auto tOnce = test("StructCodegen/aStructIsDeclaredOnceAndOnlyWhereUsed") = []
{
    auto graph = ManyUsesGraph {};
    auto sources = Sources {graph.builder.graph()};

    for (const auto* source: {&sources.metal, &sources.hlsl, &sources.glsl})
    {
        check(occurrences(*source, "struct Particle\n") == 1, *source);
        check(occurrences(*source, "struct Inner\n") == 1, *source);
        check(occurrences(*source, "struct Outer\n") == 1, *source);
        check(source->find("struct Inner\n") < source->find("struct Outer\n"));
    }

    checkAll(
        sources.metal,
        {"Outer v1 = eacpMakeOuter(0.0, eacpMakeInner(float2(0.0, 0.0), 0.0h)"});
    checkAll(sources.glsl, {"Outer v1 = Outer(0.0, Inner(vec2(0.0, 0.0), 0.0hf)"});
    expectGlslCompiles(sources.glsl, true);

    auto plain = PlainGraph {};
    auto plainSources = Sources {plain.builder.graph()};

    for (const auto* source:
         {&plainSources.metal, &plainSources.hlsl, &plainSources.glsl})
        checkNone(*source, {"eacpMake", "eacpPad", "ByteAddressBuffer"});

    check(plain.builder.graph().structCount() == 0);
};

auto tVars = test("StructCodegen/variablesAndPrivateArraysHoldStructs") = []
{
    auto graph = VarsGraph {};
    auto sources = Sources {graph.builder.graph()};

    checkAll(sources.metal,
             {"Particle v0 = eacpMakeParticle(float3(buffer0[gid].position), t0, "
              "buffer0[gid].velocity, buffer0[gid].flags);",
              "v0.mass = (v0.mass * 2.0);",
              "Particle v1[4];",
              "v1[fill1] = eacpMakeParticle(float3(0.0, 0.0, 0.0), 0.0, float2(0.0, "
              "0.0), 0u);",
              "v1[1] = eacpMakeParticle(float3(v0.position), v0.mass, v0.velocity, "
              "v0.flags);",
              "v1[min(max(int(gid), 0), 3)].velocity = (v0.velocity).yx;",
              "struct Flagged\n{\n    bool on;\n    float weight;\n};",
              "Flagged v2 = eacpMakeFlagged((gid < 3u), t0);",
              "v2.weight = (v2.weight + 1.0);",
              "int t1 = min(max((int(gid) & 3), 0), 3);",
              "(v2.on ? v1[t1].mass : (v1[t1].velocity).y)"});
    checkAll(sources.hlsl,
             {"Particle v0 = eacpMakeParticle(asfloat(buffer0.Load3((gid) * 32u)), "
              "t0,",
              "v1[1] = eacpMakeParticle(v0.position, v0.mass, v0.velocity, "
              "v0.flags);"});
    checkAll(sources.glsl,
             {"Particle v0 = Particle(buffer0[gid].position, t0, "
              "buffer0[gid].velocity, buffer0[gid].flags);",
              "Flagged v2 = Flagged((gid < 3u), t0);",
              "Particle v1[4];"});
    expectGlslCompiles(sources.glsl, true);
};

auto tHalves = test("StructCodegen/halfFieldsReadAndWriteTheirOwnBytes") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.structInputBuffer<HalfMembers>();
    auto output = builder.structOutputBuffer<HalfMembers>();
    auto id = builder.threadId();
    auto h = input[id];
    builder.write(
        output, id, HalfMembers {h.c, h.b * 2.f, h.a, h.d.yx(), h.e + 1.f, h.m});

    auto sources = Sources {builder.graph()};

    checkAll(sources.metal,
             {"struct HalfMembers\n{\n    half a;\n    uchar eacpPad0[6];\n    "
              "packed_half3 b;\n    half c;\n    half2 d;\n    float e;\n    "
              "half3x3 m;\n};",
              "half3(buffer0[gid].b)"});
    checkAll(sources.hlsl,
             {"float3(f16tof32(buffer0.Load((gid) * 48u + 8u)), "
              "f16tof32(buffer0.Load((gid) * 48u + 8u) >> 16u), "
              "f16tof32(buffer0.Load((gid) * 48u + 12u)))",
              "f16tof32(buffer0.Load((gid) * 48u + 12u) >> 16u)",
              "buffer1.InterlockedOr((gid) * 48u + 12u, f32tof16((t0.b).z) << 0u);",
              "buffer1.InterlockedOr((gid) * 48u + 12u, f32tof16(t0.c) << 16u);",
              "buffer1.Store((gid) * 48u + 20u, asuint(t0.e));",
              "buffer1.InterlockedOr((gid) * 48u + 40u, "
              "f32tof16(transpose(t0.m)[2].y) << 16u);",
              "buffer1.InterlockedOr((gid) * 48u + 44u, "
              "f32tof16(transpose(t0.m)[2].z) << 0u);"});
    checkAll(sources.glsl,
             {"struct HalfMembers\n{\n    float16_t a;\n    f16vec3 b;\n    "
              "float16_t c;\n    f16vec2 d;\n    float e;\n    f16mat3 m;\n};"});
    expectGlslCompiles(sources.glsl, true);
};

auto tFragment = test("StructCodegen/aFragmentReadsABufferOfStructs") = []
{
    auto graph = FragmentGraph {};
    auto sources = Sources {graph.builder.graph()};

    checkAll(sources.metal,
             {"device const Particle* buffer0 [[buffer(24)]]",
              "float4(float3(buffer0[0u].position), buffer0[0u].mass)"});
    checkAll(sources.hlsl,
             {"ByteAddressBuffer buffer0 : register(t8);",
              "asfloat(buffer0.Load3((0u) * 32u))"});
    checkAll(
        sources.glsl,
        {"layout(std430, set = 0, binding = 24) readonly buffer Buffer0\n{\n    "
         "Particle buffer0[];\n};",
         "fragColor = vec4(buffer0[0u].position, buffer0[0u].mass);"});
    expectGlslCompiles(sources.glsl);
};

auto tCpu = test("StructCodegen/theCpuExecutorPlansEveryGraph/runs") = []
{
    auto small = SmallGraph {};
    auto nested = NestedGraph {};
    auto vars = VarsGraph {};
    auto many = ManyUsesGraph {};

    for (const auto* graph: {&small.builder.graph(),
                             &nested.builder.graph(),
                             &vars.builder.graph(),
                             &many.builder.graph()})
        expectPlans(CpuCompute::Executor {*graph});
};
} // namespace StructCodegenTests
