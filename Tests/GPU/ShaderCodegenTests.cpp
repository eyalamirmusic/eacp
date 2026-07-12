#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// The triangle shader, authored in pure C++ via the EDSL. Mirrors the
// TriangleGen demo so the tests cover the exact path an app takes.
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

// Rotates the vertex position by a per-frame uniform angle, computed in-shader
// with sin/cos. Mirrors the RotatingTriangle demo.
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

// Samples a texture at the interpolated vertex UV. Mirrors the Texture demo.
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

// Vertex + per-instance structs for the ShaderProgram instancing test below.
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

// A struct-authored shader that pulls geometry per-vertex (slot 0) and a
// transform + colour per-instance (slots 1 and 2), mirroring what the
// Instancing demo does. Exercises ShaderProgram::instanceInput and the
// multi-slot vertex layout it assembles.
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

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Derives the MSL uniform-block declaration string from the runtime constant
// that the emitter uses (RenderPass::uniformBase / ComputePass::uniformBase).
// Bumping the constant flows into both the emitter's output and the tests'
// expectation - one source of truth, no drift.
std::string uniformDecl(int base)
{
    return "constant Uniforms& uniforms [[buffer(" + std::to_string(base) + ")]]";
}

int countOccurrences(const std::string& haystack, const std::string& needle)
{
    auto count = 0;

    for (auto found = haystack.find(needle); found != std::string::npos;
         found = haystack.find(needle, found + needle.size()))
        ++count;

    return count;
}
} // namespace

// The generated vertex layout is derived from the same input declarations that
// produce the shader source, so it cannot drift from the shader. Pure logic, no
// GPU device required.
auto tCodegenLayout = test("GPU/codegenVertexLayout") = []
{
    auto shader = makeTriangleShader();
    const auto& layout = shader.vertexLayout;

    check(layout.attributes.size() == 2);
    check(layout.attributes[0].format == VertexFormat::Float2);
    check(layout.attributes[0].offset == 0);
    check(layout.attributes[1].format == VertexFormat::Float3);
    check(layout.attributes[1].offset == (int) (sizeof(float) * 2));
    check(layout.stride == (int) (sizeof(float) * 5));

    check(shader.source.vertexEntry == "vertexMain");
    check(shader.source.fragmentEntry == "fragmentMain");
};

// ShaderProgram::instanceInput assembles a multi-slot vertex layout from the
// real CPU struct offsets: slot 0 per-vertex, the instanceInput slots
// per-instance, each with the source struct's size as its stride. The layout
// half is pure logic; the pipeline build + instance-count wiring self-skips
// without a GPU device (matches the compile tests here).
auto tShaderProgramInstancedLayout = test("GPU/shaderProgramInstancedLayout") = []
{
    auto program = InstancedProgram {};
    const auto& layout = program.vertexLayout();

    // Three bound slots: one per-vertex, two per-instance, strides taken from
    // the CPU structs (not a byte-size sum), so padded structs stay correct.
    check(program.isInstanced());
    check(layout.buffers.size() == 3);
    check(layout.buffers[0].stride == (int) sizeof(ProgVertex));
    check(layout.buffers[0].stepRate == StepRate::PerVertex);
    check(layout.buffers[1].stride == (int) sizeof(ProgInstanceTransform));
    check(layout.buffers[1].stepRate == StepRate::PerInstance);
    check(layout.buffers[2].stride == (int) sizeof(ProgInstanceColor));
    check(layout.buffers[2].stepRate == StepRate::PerInstance);

    // Every attribute routes to its slot at its real member offset.
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

// One IR emits both backends; assert each carries its backend-specific binding
// syntax. Pure string generation, runs on any host.
auto tCodegenEmitsBothBackends = test("GPU/codegenEmitsBothBackends") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    auto metal = emitMetal(builder.graph());
    auto hlsl = emitHlsl(builder.graph());

    check(contains(metal, "[[attribute(0)]]"));
    check(contains(metal, "[[position]]"));
    check(contains(metal, "vertex VertexOut vertexMain"));
    check(contains(metal, "fragment float4 fragmentMain"));

    check(contains(hlsl, "TEXCOORD0"));
    check(contains(hlsl, "SV_Position"));
    check(contains(hlsl, "SV_Target"));
};

// Feeds the generated source through the real platform shader compiler and
// builds a pipeline from the generated layout. Self-skips on hosts without a GPU
// device (matches GPUSmokeTests).
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

// A uniform<>() declaration adds a uniform block bound per-frame, and sin/cos
// are emitted as builtin calls. Built inline (like codegenEmitsBothBackends) so
// both backends can be inspected headlessly. Pure string generation.
auto tCodegenUniformEmits = test("GPU/codegenUniformEmits") = []
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
    builder.position(float4(float2(px * c - py * s, px * s + py * c), 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "struct Uniforms"));
    check(contains(metal, uniformDecl(RenderPass::uniformBase)));
    check(contains(metal, "cos(uniforms.u0)"));
    check(contains(metal, "sin(uniforms.u0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "cbuffer UniformsCB : register(b0)"));
    check(contains(hlsl, "cos(uniforms.u0)"));
    check(contains(hlsl, "sin(uniforms.u0)"));

    // The plain triangle declares no uniforms, so no block is emitted.
    auto plain = makeTriangleShader();
    check(!contains(plain.source.source, "Uniforms"));
};

// HLSL cbuffer packing only forbids straddling a 16-byte register, while the
// CPU block follows MSL struct alignment (a vec3 aligns to 16). A scalar
// followed by a vector is where they disagree: HLSL would pack the float3 at
// offset 4, the CPU writes it at 16, so the emitter must pad the cbuffer
// struct up to the CPU offsets.
auto tCodegenCbufferPadding = test("GPU/codegenHlslCbufferPadding") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto brightness = builder.uniform<Float>();
    auto tint = builder.uniform<Float3>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(tint * brightness, 1.0f));

    // One pad moves the float3 to offset 8, where the no-straddle rule bumps it
    // the rest of the way to 16; the emitter pads minimally and lets the rule
    // finish the job.
    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "    float u0;\n"
                   "    float pad0;\n"
                   "    float3 u1;\n"));

    // MSL aligns the vec3 to 16 natively, so its struct needs no padding.
    auto metal = emitMetal(builder.graph());
    check(!contains(metal, "pad"));
};

// A float2 after a float packs at 4 in HLSL but at 8 on the CPU side, so it
// pads by one scalar; vector-only blocks land identically under both rule sets
// and stay pad-free.
auto tCodegenCbufferPaddingFloat2 = test("GPU/codegenHlslCbufferPaddingFloat2") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto brightness = builder.uniform<Float>();
    auto offset = builder.uniform<Float2>();

    builder.position(float4(position + offset, 0.0f, 1.0f));
    builder.fragment(
        float4(position.x(), position.y(), brightness, builder.constant(1.0f)));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "    float u0;\n"
                   "    float pad0;\n"
                   "    float2 u1;\n"));

    auto vectors = ShaderBuilder {};
    auto vectorPosition = vectors.vertexInput<Float2>();
    auto viewport = vectors.uniform<Float2>();
    auto color = vectors.uniform<Float4>();

    vectors.position(float4(vectorPosition + viewport, 0.0f, 1.0f));
    vectors.fragment(color);

    check(!contains(emitHlsl(vectors.graph()), "pad"));
};

// Unary minus and float literal operands record IR nodes directly, with no
// constant() wrapping at the call site. Pure string generation.
auto tCodegenOperatorSugar = test("GPU/codegenOperatorSugar") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto x = position.x();

    builder.position(float4(float2(-x * 2.0f, 1.0f - x), 0.0f, 1.0f));
    builder.fragment(float4(float3(x, x, x), 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "(-((input.a0).x))"));
    check(contains(metal, " * 2.0)"));
    check(contains(metal, "(1.0 - (input.a0).x)"));
};

// Negating a negative constant emits nested parentheses, not a pre-decrement:
// rotateX(-72 degrees) bakes sin() as a negative literal and then negates it.
auto tCodegenNegatedNegative = test("GPU/codegenNegatedNegativeConstant") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto negated = -builder.constant(-0.5f);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(float2(negated, negated), float2(negated, negated)));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "(-(-0.5))"));
    check(!contains(metal, "--"));
};

// Vector constructors take any mix of handles and literals whose components
// total the width - including the previously missing float4(vec3, scalar
// handle) shape - and compile through the real shader compiler. Self-skips
// the compile half without a GPU device.
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

// An operation referenced more than once is hoisted into a named local and
// computed once per stage, so generated source stays linear in the graph size
// instead of re-inlining shared subtrees at every use. Leaf reads stay inline.
auto tCodegenSharedSubexpressions = test("GPU/codegenSharedSubexpressions") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto angle = builder.uniform<Float>();
    auto varyingColor = builder.varying(color);

    // cos/sin each feed both rotated components: one local each in the vertex
    // stage.
    auto c = cos(angle);
    auto s = sin(angle);
    auto px = position.x();
    auto py = position.y();
    builder.position(float4(float2(px * c - py * s, px * s + py * c), 0.0f, 1.0f));

    // normalize() feeds both the colour and its scale: one local in the
    // fragment stage.
    auto unit = normalize(varyingColor);
    builder.fragment(float4(unit * length(unit), 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "    float t0 = cos(uniforms.u0);\n"));
    check(contains(metal, "    float t1 = sin(uniforms.u0);\n"));
    check(countOccurrences(metal, "cos(") == 1);
    check(countOccurrences(metal, "sin(") == 1);

    check(contains(metal, "    float3 t0 = normalize(input.v0);\n"));
    check(countOccurrences(metal, "normalize(") == 1);
    check(contains(metal, "length(t0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(countOccurrences(hlsl, "cos(") == 1);
    check(countOccurrences(hlsl, "sin(") == 1);
    check(countOccurrences(hlsl, "normalize(") == 1);
};

// Intrinsics carry the canonical MSL name and translate where HLSL spells
// differently: fract -> frac, mix -> lerp; the rest are shared. Pure string
// generation.
auto tCodegenIntrinsicNames = test("GPU/codegenIntrinsicNames") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(float4(position, 0.0f, 1.0f));

    auto t = fract(varyingColor.x());
    auto shaped = smoothstep(0.0f, 1.0f, t);
    auto tinted = mix(varyingColor, normalize(varyingColor), shaped);
    auto lit = clamp(tinted * abs(varyingColor.y()), 0.0f, 1.0f);
    builder.fragment(float4(lit, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "fract("));
    check(contains(metal, "mix("));
    check(contains(metal, "smoothstep(0.0, 1.0, "));
    check(contains(metal, "clamp("));
    check(contains(metal, "abs("));
    check(contains(metal, "normalize("));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "frac("));
    check(contains(hlsl, "lerp("));
    check(contains(hlsl, "smoothstep("));
    check(!contains(hlsl, "fract("));
    check(!contains(hlsl, "mix("));
};

// Runs the whole vocabulary through the real platform shader compiler, so
// every intrinsic spelling and broadcast form is validated against the actual
// language. Self-skips without a GPU device.
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

// A uniform read only by the fragment expression binds the block to the
// fragment stage: the MSL fragment function gains the uniforms parameter and
// the vertex function drops it. The HLSL cbuffer is a global both stages
// already see. Pure string generation.
auto tCodegenFragmentUniformEmits = test("GPU/codegenFragmentUniformEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.uniform<Float4>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(color);

    auto metal = emitMetal(builder.graph());
    check(
        contains(metal, "vertex VertexOut vertexMain(VertexIn input [[stage_in]])"));
    check(contains(metal,
                   "fragment float4 fragmentMain(VertexOut input [[stage_in]],\n    "
                       + uniformDecl(RenderPass::uniformBase) + ")"));
    check(contains(metal, "return uniforms.u0;"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "cbuffer UniformsCB : register(b0)"));
    check(contains(hlsl, "return uniforms.u0;"));
};

// A uniform read by both stages puts the parameter on both Metal functions:
// one block, bound twice, one slot rule.
auto tCodegenSharedUniformEmits = test("GPU/codegenSharedUniformEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto scale = builder.uniform<Float>();

    auto scaled = float2(position.x() * scale, position.y() * scale);
    builder.position(float4(scaled, 0.0f, 1.0f));
    builder.fragment(float4(scale, scale, scale, builder.constant(1.0f)));

    auto metal = emitMetal(builder.graph());
    check(contains(metal,
                   "vertex VertexOut vertexMain(VertexIn input [[stage_in]], "
                       + uniformDecl(RenderPass::uniformBase) + ")"));
    check(contains(metal,
                   "fragment float4 fragmentMain(VertexOut input [[stage_in]],\n    "
                       + uniformDecl(RenderPass::uniformBase) + ")"));
};

// Compiles a fragment-uniform shader through the real platform shader compiler
// and builds a pipeline, exercising the uniform-bearing fragment signature.
// Self-skips without a GPU device.
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

// A texture() declaration reaches both backends with paired texture / sampler
// bindings at the same index: fragment function parameters on Metal, globals
// with t/s registers on D3D. Pure string generation.
auto tCodegenTextureEmits = test("GPU/codegenTextureEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, varyingUv));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "texture2d<float> texture0 [[texture(0)]]"));
    check(contains(metal, "sampler sampler0 [[sampler(0)]]"));
    check(contains(metal, "texture0.sample(sampler0, input.v0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "Texture2D texture0 : register(t0);"));
    check(contains(hlsl, "SamplerState sampler0 : register(s0);"));
    check(contains(hlsl, "texture0.Sample(sampler0, input.v0)"));

    // The vertex stage carries no texture parameters; only the fragment
    // signature gains them on Metal.
    check(
        contains(metal, "vertex VertexOut vertexMain(VertexIn input [[stage_in]])"));
};

// Compiles the sampling shader through the real platform shader compiler and
// builds a pipeline from its layout, exercising the texture-bearing fragment
// signature. Self-skips without a GPU device.
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

// A compute kernel authored via the EDSL: storage buffers, the thread id, a
// uniform and a store. The kernel scaffolding differs per backend (function
// parameters on Metal, globals + numthreads on D3D); the body and the implicit
// element-count guard are shared. Pure string generation.
auto tCodegenComputeEmits = test("GPU/codegenComputeEmits") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto scale = builder.uniform<Float>();
    auto gid = builder.threadId();

    builder.write(output, gid, input[gid] * scale);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "kernel void computeMain("));
    check(contains(metal, "device const float* buffer0 [[buffer(0)]]"));
    check(contains(metal, "device float* buffer1 [[buffer(1)]]"));
    check(contains(metal, uniformDecl(ComputePass::uniformBase)));
    check(contains(metal, "uint gid [[thread_position_in_grid]]"));
    check(contains(metal, "uint count;"));
    check(contains(metal, "if (gid >= uniforms.count)"));
    check(contains(metal, "buffer1[gid] = (buffer0[gid] * uniforms.u0);"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "StructuredBuffer<float> buffer0 : register(t0);"));
    check(contains(hlsl, "RWStructuredBuffer<float> buffer1 : register(u1);"));
    check(contains(hlsl, "cbuffer UniformsCB : register(b0)"));
    check(contains(hlsl, "[numthreads(64, 1, 1)]"));
    check(contains(hlsl, "uint3 threadId : SV_DispatchThreadID"));
    check(contains(hlsl, "uint gid = threadId.x;"));
    check(contains(hlsl, "if (gid >= uniforms.count)"));
    check(contains(hlsl, "buffer1[gid] = (buffer0[gid] * uniforms.u0);"));
};

// A buffer element read more than once hoists into a named local like any
// other shared operation, and build() marks the source as compute with the
// kernel entry point and no vertex layout. A kernel without user uniforms
// still gets the block: the implicit count lives there.
auto tCodegenComputeSharedRead = test("GPU/codegenComputeSharedRead") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto gid = builder.threadId();

    auto value = input[gid];
    builder.write(output, gid, value * value);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "struct Uniforms"));
    check(contains(metal, "uint count;"));
    check(contains(metal, "    float t0 = buffer0[gid];\n"));
    check(contains(metal, "buffer1[gid] = (t0 * t0);"));
    check(countOccurrences(metal, "buffer0[gid]") == 1);

    auto shader = builder.build();
    check(shader.source.isCompute());
    check(shader.source.computeEntry == "computeMain");
    check(shader.vertexLayout.attributes.size() == 0);
};

// Index arithmetic: uint operators against uint values and integer literals
// (recorded as uint constant nodes), a uint uniform read inside an index
// expression, and uint min/max. The spelling is shared by both backends.
auto tCodegenComputeIndexArithmetic = test("GPU/codegenComputeIndexArithmetic") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto length = builder.uniform<UInt>();
    auto gid = builder.threadId();

    auto previous = input[(gid + length - 1u) % length];
    auto next = input[min(gid + 1u, max(length, 1u) - 1u)];
    builder.write(output, gid * 2u, (previous + next) / 2.0f);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint u0;"));
    check(contains(metal, "buffer0[(((gid + uniforms.u0) - 1u) % uniforms.u0)]"));
    check(contains(metal, "min((gid + 1u), (max(uniforms.u0, 1u) - 1u))"));
    check(contains(metal, "buffer1[(gid * 2u)] = "));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "uint u0;"));
    check(contains(hlsl, "buffer0[(((gid + uniforms.u0) - 1u) % uniforms.u0)]"));
    check(contains(hlsl, "buffer1[(gid * 2u)] = "));
};

// Feeds an EDSL compute kernel (including the toFloat(threadId) cast) through
// the real platform shader compiler and builds a compute pipeline. Self-skips
// without a GPU device.
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

// Compiles the rotating shader (with its uniform block) through the real
// platform shader compiler. Self-skips without a GPU device.
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
