#include "CodegenCommon.h"

#include <eacp/GPU/Codegen/WgslBindings.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

void expectContains(
    const std::string& source,
    const std::string& needle,
    const std::source_location& location = std::source_location::current())
{
    check(contains(source, needle), "missing: " + needle + "\n" + source, location);
}

std::string at(int binding)
{
    return "@group(0) @binding(" + std::to_string(binding) + ") ";
}

// What a Cows prop is drawn with: a mesh vertex beside thirteen vec4s of
// per-instance data - a model matrix, a normal matrix, a colour, a sway and a
// shadow-space matrix - lit, fogged and shadowed.
struct InstancedScene
{
    InstancedScene()
    {
        auto position = builder.vertexInput<Float3>();
        auto normal = builder.vertexInput<Float3>();

        auto model0 = builder.instanceInput<Float4>();
        auto model1 = builder.instanceInput<Float4>();
        auto model2 = builder.instanceInput<Float4>();
        auto model3 = builder.instanceInput<Float4>();
        auto normal0 = builder.instanceInput<Float4>();
        auto normal1 = builder.instanceInput<Float4>();
        auto normal2 = builder.instanceInput<Float4>();
        auto color = builder.instanceInput<Float4>();
        auto sway = builder.instanceInput<Float4>();
        auto light0 = builder.instanceInput<Float4>();
        auto light1 = builder.instanceInput<Float4>();
        auto light2 = builder.instanceInput<Float4>();
        auto light3 = builder.instanceInput<Float4>();

        auto viewProjection = builder.uniform<Float4x4>();
        auto lightDirection = builder.uniform<Float3>();
        auto time = builder.uniform<Float>();
        auto fogColor = builder.uniform<Float4>();

        auto model = float4x4(model0, model1, model2, model3);
        auto normalMatrix = float3x3(normal0.xyz(), normal1.xyz(), normal2.xyz());
        auto bend = sin(time * sway.x() + sway.y()) * sway.z() * position.y();
        auto world = model * float4(position + float3(bend, 0.0f, 0.0f), 1.0f);
        auto shadowSpace = float4x4(light0, light1, light2, light3) * world;

        auto worldNormal = builder.varying(normalize(normalMatrix * normal));
        auto tint = builder.varying(color);
        auto shadowCoord = builder.varying(shadowSpace.xyz() / shadowSpace.w());
        auto depth = builder.varying(world.z());

        builder.position(viewProjection * world);

        auto shadowMap = builder.texture();

        auto lit = builder.var(0.0f);
        auto dx = builder.var(-1);

        builder.loop(
            dx <= 1,
            [&]
            {
                auto dy = builder.var(-1);

                builder.loop(
                    dy <= 1,
                    [&]
                    {
                        auto offset =
                            float2(toFloat(dx.get()), toFloat(dy.get())) / 2048.0f;
                        auto stored =
                            sample(shadowMap, shadowCoord.xy() + offset).x();

                        builder.ifThen(shadowCoord.z() - 0.002f <= stored,
                                       [&] { lit += 1.0f / 9.0f; });

                        dy = dy.get() + 1;
                    });

                dx = dx.get() + 1;
            });

        auto diffuse = max(dot(worldNormal, -lightDirection), 0.0f);
        auto edge = fwidth(depth);
        auto fog = smoothstep(20.0f, 80.0f, depth + edge);
        auto shaded = tint.xyz() * (0.35f + 0.65f * diffuse * lit.get());

        builder.discardBelow(tint.w(), 0.5f);
        builder.fragment(mix(float4(shaded, tint.w()), fogColor, fog));
    }

    ShaderBuilder builder;
};

// The glow Cows adds over the scene with an additive blend: the blend is the
// pipeline's, so the shader is a textured quad with a pulse.
GeneratedShader makeAdditiveGlow(ShaderBuilder& builder)
{
    auto corner = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto centre = builder.instanceInput<Float4>();
    auto pulse = builder.uniform<Float>();

    builder.position(float4(corner * centre.z() + centre.xy(), 0.0f, 1.0f));

    auto glow = builder.texture({TextureFilter::Linear, TextureAddressMode::Clamp});
    auto carried = builder.varying(uv);
    auto strength = sample(glow, carried).x() * (0.5f + 0.5f * sin(pulse));

    builder.fragment(float4(strength, strength * 0.8f, strength * 0.4f, 1.0f));
    return builder.build();
}
} // namespace

auto tWgslRenderShape = test("Wgsl/renderModuleShape") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto carried = builder.varying(color);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(carried, 1.0f));

    auto wgsl = emitWgsl(builder.graph());

    check(wgsl.rfind("diagnostic(off, derivative_uniformity);\n", 0) == 0);
    expectContains(wgsl, "    @location(0) a0: vec2f,\n");
    expectContains(wgsl, "    @location(1) a1: vec3f,\n");
    expectContains(wgsl, "    @builtin(position) position: vec4f,\n");
    expectContains(wgsl, "    @location(0) v0: vec3f,\n");
    expectContains(wgsl, "@vertex\nfn vertexMain(input: VertexIn) -> VertexOut\n");
    expectContains(wgsl, "    output.position = vec4f(input.a0, 0.0, 1.0);\n");
    expectContains(wgsl,
                   "@fragment\nfn fragmentMain(input: VertexOut) -> "
                   "@location(0) vec4f\n");
    expectContains(wgsl, "    return vec4f(input.v0, 1.0);\n");

    expectWgslValid(builder.graph());
};

// std140 and WGSL's uniform address space both put a scalar straight after a
// vec3, where MSL - and so the CPU block - leaves the vec3 a whole 16 bytes.
auto tWgslUniformPads = test("Wgsl/uniformBlockMatchesTheCpuLayout") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto direction = builder.uniform<Float3>();
    auto scale = builder.uniform<Float>();
    auto matrix = builder.uniform<Float4x4>();
    auto cell = builder.uniform<Int2>();

    builder.position(matrix * float4(position * scale, 0.0f, 1.0f));
    builder.fragment(float4(direction, toFloat(cell.x())));

    auto wgsl = emitWgsl(builder.graph());

    expectContains(wgsl,
                   "struct Uniforms\n{\n    u0: vec3f,\n    pad0: f32,\n"
                   "    u1: f32,\n    u2: mat4x4f,\n    u3: vec2i,\n};\n");
    expectContains(wgsl,
                   at(wgslUniformBinding) + "var<uniform> uniforms: Uniforms;");

    expectWgslValid(builder.graph());
};

// Integers cross the stage boundary flat, as they must in every dialect.
auto tWgslFlatVaryings = test("Wgsl/integerVaryingsAreFlat") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto index = builder.uniform<UInt>();
    auto carried = builder.varying(index * 2u);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(toFloat(carried), 0.0f, 0.0f, 1.0f));

    auto wgsl = emitWgsl(builder.graph());
    expectContains(wgsl, "@location(0) @interpolate(flat) v0: u32,");

    expectWgslValid(builder.graph());
};

// One sampler per texture at 32 + slot, textures at 8 + slot, render storage
// buffers at 24 + slot; a depth texture is texture_depth_2d.
auto tWgslRenderBindings = test("Wgsl/renderBindings") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.varying(position);
    auto image = builder.texture();
    auto sky = builder.cubeTexture();
    auto depth = builder.depthTexture();
    auto data = builder.inputBuffer();
    auto record = builder.uniform<UInt>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, uv) + sample(sky, float3(uv, 1.0f))
                     + sample(image, uv) * sample(depth, uv) * data[record]);

    auto wgsl = emitWgsl(builder.graph());

    expectContains(wgsl,
                   at(wgslTextureBinding(0)) + "var texture0: texture_2d<f32>;");
    expectContains(wgsl, at(wgslSamplerBinding(0)) + "var sampler0: sampler;");
    expectContains(wgsl,
                   at(wgslTextureBinding(1)) + "var texture1: texture_cube<f32>;");
    expectContains(wgsl, at(wgslSamplerBinding(1)) + "var sampler1: sampler;");
    expectContains(wgsl,
                   at(wgslTextureBinding(2)) + "var texture2: texture_depth_2d;");
    expectContains(
        wgsl, at(wgslBufferBinding(0)) + "var<storage, read> buffer0: array<f32>;");
    expectContains(wgsl, "textureSample(texture0, sampler0, input.v0)");
    expectContains(wgsl, "textureSample(texture2, sampler2, input.v0)");

    check(wgslTextureBinding(0) == 8);
    check(wgslSamplerBinding(0) == 32);
    check(wgslBufferBinding(0) == 24);
    check(wgslComputeTextureBinding(0) == 8);
    check(wgslComputeUniformBinding == 16);

    expectWgslValid(builder.graph());
};

// textureSample needs derivatives, which only the fragment stage has; an
// explicit level is textureSampleLevel everywhere, an integer one on depth.
auto tWgslSampleLevels = test("Wgsl/samplingOutsideTheFragmentStage") = []
{
    auto render = ShaderBuilder {};

    auto position = render.vertexInput<Float2>();
    auto uv = render.varying(position);
    auto image = render.texture();

    render.position(float4(position, 0.0f, 1.0f));
    render.fragment(sample(image, uv, 2.0f)
                    + fetch(image, int2(render.integer(1), 2)));

    auto wgsl = emitWgsl(render.graph());
    expectContains(wgsl, "textureSampleLevel(texture0, sampler0, input.v0, 2.0)");
    expectContains(wgsl, "textureLoad(texture0, vec2i(1, 2), 0)");
    expectWgslValid(render.graph());

    auto compute = ShaderBuilder {};

    auto output = compute.outputBuffer();
    auto source = compute.texture();
    auto id = compute.threadId();

    compute.write(output, id, sample(source, float2(toFloat(id), 0.5f)).x());

    auto kernel = emitWgsl(compute.graph());
    expectContains(kernel, "textureSampleLevel(texture0, sampler0, ");
    check(!contains(kernel, "textureSample("));
    expectContains(
        kernel, at(wgslComputeTextureBinding(0)) + "var texture0: texture_2d<f32>;");
    expectContains(kernel, at(wgslSamplerBinding(0)) + "var sampler0: sampler;");
    expectWgslValid(compute.graph());
};

// WGSL converts nothing implicitly and has no ?:, so these are the spellings
// the other dialects get for free.
auto tWgslOperators = test("Wgsl/operatorsWithoutImplicitConversions") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto bits = builder.uniform<UInt>();
    auto cell = builder.uniform<Int2>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto shifted = (cell << 1) & 3;
    auto signedShift = builder.integer(1) << cell.x();
    auto remainder = cell.y() % 3;
    auto mask = bits >> 4u;
    auto picked = select(position.x() > 0.5f, position, float2(position.y(), 0.75f));
    auto rounded = round(position * 3.5f);

    builder.fragment(float4(toFloat(shifted.x() + signedShift + remainder),
                            toFloat(mask),
                            picked.x() + rounded.y(),
                            1.0f));

    auto wgsl = emitWgsl(builder.graph());

    expectContains(wgsl, "(uniforms.u1 << vec2u(1))");
    expectContains(wgsl, " & vec2i(3))");
    expectContains(wgsl, "(1 << u32((uniforms.u1).x))");
    expectContains(wgsl, "((uniforms.u1).y % 3)");
    expectContains(wgsl, "select(vec2f((input.v0).y, 0.75), input.v0, ");
    expectContains(wgsl, "fn eacpRound2(x: vec2f) -> vec2f");
    expectContains(wgsl, "eacpRound2(");

    expectWgslValid(builder.graph());
};

// The compute scaffolding: builtins, workgroup size, storage access modes,
// atomics, workgroup memory and the barrier, a written texture.
auto tWgslCompute = test("Wgsl/computeKernel") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto counter = builder.atomicBuffer();
    auto target = builder.writableTexture();
    auto tile = builder.shared<Float>(64);

    builder.setThreadGroupShape({64, 1, 1});

    auto id = builder.threadId();
    auto lane = builder.localId();

    builder.write(tile, lane, input[id]);
    builder.barrier();

    auto total = builder.groupSum(tile[lane]);
    auto slot = builder.atomicAdd(counter, 0u, 1u);

    builder.write(output, id, total + toFloat(slot) + toFloat(builder.gridCount()));
    builder.write(target, id, lane, float4(total, 0.0f, 0.0f, 1.0f));

    auto wgsl = emitWgsl(builder.graph());

    expectContains(wgsl,
                   "@compute @workgroup_size(64, 1, 1)\nfn computeMain("
                   "@builtin(global_invocation_id) threadId: vec3u");
    expectContains(wgsl, "@builtin(local_invocation_index) groupLane: u32");
    expectContains(wgsl, "    let gid: u32 = threadId.x;");
    expectContains(wgsl,
                   at(wgslComputeBufferBinding(0))
                       + "var<storage, read> buffer0: array<f32>;");
    expectContains(wgsl,
                   at(wgslComputeBufferBinding(1))
                       + "var<storage, read_write> buffer1: array<f32>;");
    expectContains(wgsl,
                   at(wgslComputeBufferBinding(2))
                       + "var<storage, read_write> buffer2: array<atomic<u32>>;");
    expectContains(wgsl,
                   at(wgslComputeTextureBinding(0))
                       + "var texture0: texture_storage_2d<rgba8unorm, write>;");
    expectContains(wgsl, "var<workgroup> s0: array<f32, 64>;");
    expectContains(wgsl, "var<workgroup> groupScratch: array<f32, 64>;");
    expectContains(wgsl, "workgroupBarrier();");
    expectContains(wgsl, "atomicAdd(&buffer2[0u], 1u)");
    expectContains(wgsl, "textureStore(texture0, vec2u(gid, lid), ");
    expectContains(
        wgsl, at(wgslComputeUniformBinding) + "var<uniform> uniforms: Uniforms;");

    expectWgslValid(builder.graph());
};

auto tWgslInstancedScene = test("Wgsl/cowsInstancedScene") = []
{
    auto scene = InstancedScene {};
    const auto& graph = scene.builder.graph();

    auto wgsl = emitWgsl(graph);

    expectContains(wgsl, "    @location(14) a14: vec4f,\n");
    expectContains(wgsl, "mat4x4f(input.a2, input.a3, input.a4, input.a5)");
    expectContains(wgsl, "fwidth(");
    expectContains(wgsl, "textureSample(texture0, sampler0, ");
    expectContains(wgsl, "while (");
    expectContains(wgsl, "        discard;\n");

    expectWgslValid(graph);
    expectGlslCompiles(graph);
};

auto tWgslAdditiveGlow = test("Wgsl/cowsAdditiveGlow") = []
{
    auto builder = ShaderBuilder {};
    makeAdditiveGlow(builder);

    expectWgslValid(builder.graph());
};

// Every intrinsic the EDSL names, through a validator rather than a string.
auto tWgslIntrinsics = test("Wgsl/intrinsics") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float3>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 1.0f));

    auto a = carried;
    auto s = carried.x();
    auto value = sin(a) + cos(a) + tan(a) + asin(a) + acos(a) + atan(a) + atan2(a, s)
                 + sinh(a) + cosh(a) + tanh(a) + exp(a) + exp2(a) + log(a) + log2(a)
                 + log10(a) + sqrt(a) + rsqrt(a) + abs(a) + sign(a) + floor(a)
                 + ceil(a) + trunc(a) + round(a) + fract(a) + min(a, s)
                 + max(a, 0.5f) + clamp(a, 0.0f, 1.0f) + mix(a, a, s) + step(s, a)
                 + smoothstep(0.0f, 1.0f, a) + pow(a, s) + dfdx(a) + dfdy(a)
                 + fwidth(a) + reflect(a, normalize(a))
                 + refract(a, normalize(a), 0.5f) + faceforward(a, a, a)
                 + cross(a, a);

    builder.fragment(float4(value, length(a) + distance(a, a) + dot(a, a)));

    expectWgslValid(builder.graph());
};
