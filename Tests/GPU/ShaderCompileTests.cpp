#include "Common.h"

// Every source here is built into a real pipeline, the only check that the
// language has the builtin the emitter named.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
GeneratedShader makeTriangleShader()
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    return builder.build();
}

GeneratedShader makeRotatingShader()
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto angle = builder.uniform<Float>();
    auto varyingColor = builder.varying(color);

    auto c = cos(angle);
    auto s = sin(angle);
    auto px = position.x();
    auto py = position.y();
    auto rotated = float2(px * c - py * s, px * s + py * c);

    builder.position(float4(rotated, 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    return builder.build();
}

GeneratedShader makeTexturedShader()
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, varyingUv));

    return builder.build();
}

struct ProgVertex
{
    float position[2];
    float uv[2];
};

struct ProgInstanceTransform
{
    float center[2];
    float scale;
};

struct ProgInstanceColor
{
    float color[3];
};

struct InstancedProgram final : ShaderProgram
{
    Uniform<Float> time;

    EACP_SHADER(time)

    InstancedProgram() { compile(); }

    void define() override
    {
        auto position = vertexInput(&ProgVertex::position);
        auto uv = vertexInput(&ProgVertex::uv);
        auto center = instanceInput(&ProgInstanceTransform::center, 1);
        auto scale = instanceInput(&ProgInstanceTransform::scale, 1);
        auto color = instanceInput(&ProgInstanceColor::color, 2);

        auto placed = position * (scale * time);
        setPosition(
            float4(placed.x() + center.x(), placed.y() + center.y(), 0.f, 1.f));
        setFragment(float4(varying(color) * varying(uv).y(), 1.f));
    }
};

// The shape that bound short on Metal until the upload walk padded the total.
struct OffBoundaryProgram final : ShaderProgram
{
    Uniform<Float2> scale;
    Uniform<Float2> shift;
    Uniform<Float> fade;

    EACP_SHADER(scale, shift, fade)

    OffBoundaryProgram() { compile(); }

    void define() override
    {
        auto position = vertexInput(&ProgVertex::position);
        auto x = position.x() * scale.x() + shift.x();
        auto y = position.y() * scale.y() + shift.y();
        setPosition(float4(x, y, 0.0f, 1.0f));
        setFragment(float4(fade, fade, fade, 1.0f));
    }
};

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

auto tShaderProgramInstancedLayout = test("GPU/shaderProgramInstancedLayout") = []
{
    auto program = InstancedProgram {};
    const auto& layout = program.vertexLayout();

    check(program.isInstanced());
    check(layout.buffers.size() == 3);
    check(layout.buffers[0].stride == (int) sizeof(ProgVertex));
    check(layout.buffers[0].stepRate == StepRate::PerVertex);
    check(layout.buffers[1].stride == (int) sizeof(ProgInstanceTransform));
    check(layout.buffers[1].stepRate == StepRate::PerInstance);
    check(layout.buffers[2].stride == (int) sizeof(ProgInstanceColor));
    check(layout.buffers[2].stepRate == StepRate::PerInstance);

    const auto& attrs = layout.attributes;
    check(attrs.size() == 5);
    check(attrs[0].bufferIndex == 0 && attrs[0].offset == 0);
    check(attrs[1].bufferIndex == 0 && attrs[1].offset == (int) sizeof(float) * 2);
    check(attrs[2].bufferIndex == 1 && attrs[2].offset == 0);
    check(attrs[3].bufferIndex == 1 && attrs[3].offset == (int) sizeof(float) * 2);
    check(attrs[4].bufferIndex == 2 && attrs[4].offset == 0);

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const ProgVertex verts[3] = {
        {{0.f, 1.f}, {0.5f, 1.f}},
        {{-1.f, -1.f}, {0.f, 0.f}},
        {{1.f, -1.f}, {1.f, 0.f}},
    };
    const ProgInstanceTransform transforms[4] = {
        {{-0.5f, 0.f}, 0.2f},
        {{0.5f, 0.f}, 0.2f},
        {{0.f, 0.5f}, 0.2f},
        {{0.f, -0.5f}, 0.2f},
    };
    const ProgInstanceColor colors[4] = {
        {{1.f, 0.f, 0.f}},
        {{0.f, 1.f, 0.f}},
        {{0.f, 0.f, 1.f}},
        {{1.f, 1.f, 0.f}},
    };

    program.setVertices(verts);
    program.setInstances(1, transforms);
    program.setInstances(2, colors);
    check(program.instanceCount() == 4);

    program.prepare(1);
    check(program.pipeline().isValid());
};

// Metal validates the bound length against the padded struct size.
auto tShaderProgramPadsUniformBlock = test("GPU/shaderProgramPadsUniformBlock") = []
{
    auto program = OffBoundaryProgram {};

    check(program.uniformByteSize() == 24);
};

auto tCodegenCompiles = test("GPU/codegenCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto shader = makeTriangleShader();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

// Only * and / broadcast a scalar before, so `uv + time` did not compile.
auto tCodegenScalarBroadcast = test("GPU/codegenScalarHandleBroadcast") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto amount = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position + amount, 0.0f, 1.0f));
    builder.fragment(
        float4((carried - amount) * amount, (amount / carried).x(), 1.0f));

    auto metal = emitMetal(builder.graph());

    // Order is kept as written, which matters for the two that do not commute.
    check(contains(metal, "(input.a0 + uniforms.u0)"));
    check(contains(metal, "- uniforms.u0)"));
    check(contains(metal, "* uniforms.u0)"));
    check(contains(metal, "(uniforms.u0 / "));

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto library = device.makeShaderLibrary(builder.build().source);
    check(library.isValid());
};

// Including the float4(vec3, scalar handle) shape that used to be missing.
auto tCodegenMixedConstructors = test("GPU/codegenMixedConstructors") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto varyingColor = builder.varying(color);

    auto lifted = float3(position.x(), position);
    builder.position(float4(1.0f - lifted.x(), lifted.y(), 0.5f, 1));
    builder.fragment(float4(varyingColor, length(varyingColor)));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "float3((input.a0).x, input.a0)"));
    check(contains(metal, ", 0.5, 1.0)"));
    check(contains(metal, "float4(input.v0, length(input.v0))"));

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

// MSL and HLSL have no inverse, which is why only two builtins are here.
auto tCodegenMatrixTransposeCompiles =
    test("GPU/codegenMatrixTransposeCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto angle = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto carried = builder.varying(position);

    auto rotation =
        float2x2(float2(cos(angle), sin(angle)), float2(-sin(angle), cos(angle)));

    auto basis = float3x3(float3(carried, 1.0f),
                          float3(0.0f, 1.0f, builder.constant(0.0f)),
                          float3(0.0f, builder.constant(0.0f), 1.0f));

    auto turned = transpose(rotation) * carried;
    auto lit = transpose(basis) * float3(carried, 1.0f);

    builder.fragment(float4(turned, determinant(basis) * lit.z(), 1.0f));

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    check(device.makeRenderPipeline(descriptor).isValid());
};

auto tCodegenLiteralArgumentsCompile =
    test("GPU/codegenLiteralArgumentsCompile") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto width = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto carried = builder.varying(position);

    auto rotation =
        float2x2(float2(cos(width), sin(width)), float2(-sin(width), cos(width)));

    auto turned = carried * rotation;

    auto edge = smoothstep(0.0f, width, length(turned));
    auto band = mix(0.5f, 1.0f, edge) * step(turned.x(), 0.0f);
    auto held = clamp(min(0.0f, turned.y()) + max(-1.0f, band), 0.0f, width);

    builder.fragment(float4(edge, band, held, pow(2.0f, width)));

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    check(device.makeRenderPipeline(descriptor).isValid());
};

auto tCodegenControlFlowCompiles = test("GPU/codegenControlFlowCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto time = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto origin = float3(builder.constant(0.0f), 0.0f, -3.0f);
    auto direction = normalize(float3(carried, 1.0f));

    auto travelled = builder.var(0.0f);
    auto steps = builder.var(0.0f);
    auto hit = builder.var(false);

    builder.loop(steps < 64.0f,
                 [&]
                 {
                     steps += 1.0f;

                     auto distance = length(origin + direction * travelled.get())
                                     - (1.0f + sin(time) * 0.1f);

                     builder.ifThen(distance < 0.001f,
                                    [&]
                                    {
                                        hit = builder.boolean(true);
                                        builder.breakLoop();
                                    });

                     travelled += distance;
                 });

    auto shade = exp(-travelled.get() * 0.3f);
    builder.fragment(float4(select(hit, shade, 0.0f),
                            shade,
                            select(steps > 32.0f, shade, 1.0f - shade),
                            1.0f));

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

auto tCodegenIntrinsicsCompile = test("GPU/codegenIntrinsicsCompile") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto normal = builder.vertexInput<Float3>();
    auto angle = builder.uniform<Float>();

    auto swirled = float2(position.x() * cos(angle) - position.y() * sin(angle),
                          position.x() * sin(angle) + position.y() * cos(angle));
    auto lifted = swirled * min(pow(abs(angle), 2.0f) + 0.25f, 1.0f);
    builder.position(float4(lifted, 0.0f, 1.0f));

    auto unit = normalize(builder.varying(normal));
    auto up = float3(
        builder.constant(0.0f), builder.constant(0.0f), builder.constant(1.0f));
    auto facing = abs(dot(unit, cross(unit, up) + up));
    auto rim = pow(clamp(-facing + 1.0f, 0.0f, 1.0f), 2.0f);
    auto banded = step(0.5f, fract(facing * 4.0f));
    auto soft = smoothstep(0.0f, 1.0f, mix(rim, banded, 0.5f));
    auto stepped = floor(facing * 3.0f) / 3.0f;
    auto grey = max(min(sqrt(length(unit) * soft) * stepped, 1.0f), 0.0f);
    auto biased = unit * 0.5f + 0.5f;
    builder.fragment(float4(biased * grey, 1.0f));

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

auto tCodegenTranscendentalsCompile = test("GPU/codegenTranscendentalsCompile") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto normal = builder.vertexInput<Float3>();
    auto eta = builder.uniform<Float>();

    builder.position(float4(position.yx(), 0.0f, 1.0f));

    auto unit = normalize(builder.varying(normal));
    auto angle = atan2(unit.y(), unit.x());
    auto swept =
        tan(clamp(asin(unit.z()) + acos(unit.x()) + atan(angle), -1.0f, 1.0f));
    auto tiled =
        mod(swept, 2.0f) + mod(unit, 0.5f).x() + mod(unit.zyx(), unit.xzy()).y();
    auto curve = exp(-log(exp2(log2(abs(swept) + 1.0f)))) * rsqrt(abs(tiled) + 1.0f);
    auto edged = fwidth(curve) + dfdx(curve) + dfdy(curve);
    auto quantised = ceil(curve) + trunc(curve) + round(curve) + sign(curve);

    auto bounced = reflect(unit, unit) + refract(unit, unit, eta)
                   + faceforward(unit, unit, unit);

    auto grey = clamp(distance(unit, bounced) + edged + quantised, 0.0f, 1.0f);
    builder.fragment(float4(bounced.zyx() * grey, 1.0f));

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

auto tCodegenFragmentUniformCompiles =
    test("GPU/codegenFragmentUniformCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.uniform<Float4>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(color);

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

auto tCodegenTextureCompiles = test("GPU/codegenTextureCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto shader = makeTexturedShader();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

auto tCodegenSampleLevelAndFetchCompile =
    test("GPU/codegenSampleLevelAndFetchCompile") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);
    auto level = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, varyingUv, level) + fetch(image, varyingUv));

    auto shader = builder.build();
    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());
};

auto tCodegenBufferReadCompiles = test("GPU/codegenBufferReadCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto palette = builder.inputBuffer();
    auto record = builder.uniform<UInt>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(palette.read3(record), 1.0f));

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());
};

auto tCodegenComputeCompiles = test("GPU/codegenComputeCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto scale = builder.uniform<Float>();
    auto gid = builder.threadId();

    builder.write(output, gid, input[gid] * scale + toFloat(gid));

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto pipeline = device.makeComputePipeline(library);
    check(pipeline.isValid());
};

auto tCodegenIntegersCompile = test("GPU/codegenIntegersCompile") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto time = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto palette = builder.array(float3(builder.constant(0.1f), 0.1f, 0.2f),
                                 float3(builder.constant(0.9f), 0.4f, 0.2f),
                                 float3(builder.constant(0.2f), 0.8f, 0.6f),
                                 float3(builder.constant(1.0f), 0.9f, 0.7f));

    auto raw = toInt(carried.x() * 4.0f);
    auto masked = raw & 3;
    auto clamped = min(max(raw, 0), 3);

    auto step = builder.var(0);

    builder.loop(
        step < 4,
        [&]
        { builder.ifThen(step % 2 == 0, [&] { step += 2; }, [&] { step += 1; }); });

    auto shade = toFloat(step.get() + (masked << 1) - (clamped >> 1)) * 0.05f;
    auto color = palette[masked] + palette[clamped] * shade + sin(time) * 0.0f;

    builder.fragment(float4(color, 1.0f));

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

auto tCodegenVectorTypesCompile = test("GPU/codegenVectorTypesCompile") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto origin = builder.uniform<Int2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto cell =
        min(max(toInt(carried * 32.0f) - origin, int2(builder.integer(0), 0)),
            int2(builder.integer(7), 7));

    auto checker = toFloat((cell.x() + cell.y()) % 2);

    auto inside = all(carried < float2(builder.constant(0.75f), 0.75f));
    auto touching = any(abs(cell) == int2(builder.integer(3), 3));

    auto shade = builder.var(checker);

    builder.ifThen(inside && !touching, [&] { shade = shade() * 0.5f; });

    builder.fragment(float4(shade(), shade(), shade(), 1.0f));

    auto shader = builder.build();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};

auto tCodegenUniformCompiles = test("GPU/codegenUniformCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto shader = makeRotatingShader();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};
