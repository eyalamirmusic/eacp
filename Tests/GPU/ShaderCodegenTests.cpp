#include "Common.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// Mirrors the TriangleGen demo, so the tests cover the path an app takes.
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

// Mirrors the RotatingTriangle demo.
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

// Mirrors the Texture demo.
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

// Geometry per-vertex (slot 0) and a transform + colour per-instance (slots 1
// and 2), mirroring the Instancing demo.
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

// Members stopping 4 bytes short of the block's 8-byte alignment. Regression:
// this bound short on Metal, and the validation layer aborted the first draw.
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

// Derived from the constant the emitter uses, so bumping it cannot leave this
// expectation behind.
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

auto tShaderProgramInstancedLayout = test("GPU/shaderProgramInstancedLayout") = []
{
    auto program = InstancedProgram {};
    const auto& layout = program.vertexLayout();

    // Strides are taken from the CPU structs, not a byte-size sum, so a padded
    // struct stays correct.
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

// Two Float2s and a Float stop at 20 bytes, but MSL pads the struct to the
// widest member's 8-byte alignment and Metal validates the bound length.
auto tShaderProgramPadsUniformBlock = test("GPU/shaderProgramPadsUniformBlock") = []
{
    auto program = OffBoundaryProgram {};

    check(program.uniformByteSize() == 24);
};

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

    auto plain = makeTriangleShader();
    check(!contains(plain.source.source, "Uniforms"));
};

// HLSL only forbids straddling a 16-byte register while the CPU block follows
// MSL alignment, so a scalar before a vector is where the two disagree.
auto tCodegenCbufferPadding = test("GPU/codegenHlslCbufferPadding") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto brightness = builder.uniform<Float>();
    auto tint = builder.uniform<Float3>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(tint * brightness, 1.0f));

    // One pad moves the float3 to offset 8, where the no-straddle rule bumps it
    // the rest of the way: the emitter pads minimally.
    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "    float u0;\n"
                   "    float pad0;\n"
                   "    float3 u1;\n"));

    auto metal = emitMetal(builder.graph());
    check(!contains(metal, "pad"));
};

// A float2 after a float packs at 4 in HLSL but at 8 on the CPU side; a
// vector-only block lands identically under both rule sets.
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

// Regression: only * and / broadcast a scalar handle, so `uv + time` did not
// compile while `uv * time` did.
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

// rotateX(-72 degrees) bakes sin() as a negative literal and then negates it,
// which must not emit a pre-decrement.
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

// Any mix of handles and literals whose components total the width, including
// the previously missing float4(vec3, scalar handle) shape.
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

// Hoisting keeps the generated source linear in the graph size instead of
// re-inlining shared subtrees at every use. Leaf reads stay inline.
auto tCodegenSharedSubexpressions = test("GPU/codegenSharedSubexpressions") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto angle = builder.uniform<Float>();
    auto varyingColor = builder.varying(color);

    // cos/sin each feed both rotated components: one local each.
    auto c = cos(angle);
    auto s = sin(angle);
    auto px = position.x();
    auto py = position.y();
    builder.position(float4(float2(px * c - py * s, px * s + py * c), 0.0f, 1.0f));

    // normalize() feeds both the colour and its scale: one local.
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
// differently: fract -> frac, mix -> lerp.
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

// A literal is legal in any argument position GLSL allows one, and what this
// pins is the position: each of these means something else if it moves.
auto tCodegenLiteralArguments = test("GPU/codegenLiteralArguments") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto width = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto carried = builder.varying(position);

    auto edge = smoothstep(0.0f, width, length(carried));
    auto lowest = min(0.0f, carried.x());
    auto gate = step(carried.y(), 0.0f);
    auto curve = pow(2.0f, width);
    auto blend = mix(0.5f, 1.0f, edge);
    auto held = clamp(carried.x() + carried.y(), 0.0f, width);
    auto raised = max(-1.0f, carried.y());

    // clamp was the last one still insisting on a handle in front.
    auto pinned = clamp(0.02f, 2.0f, width);
    auto bounded = clamp(0.25f, carried.x(), width);

    builder.fragment(
        float4(lowest + gate + pinned, curve * blend, held + bounded, raised));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "smoothstep(0.0, uniforms.u0, "));
    check(contains(metal, "min(0.0, "));
    check(contains(metal, "step((input.v0).y, 0.0)"));
    check(contains(metal, "pow(2.0, uniforms.u0)"));
    check(contains(metal, "mix(0.5, 1.0, "));
    check(contains(metal, ", 0.0, uniforms.u0)"));
    check(contains(metal, "max(-1.0, "));
    check(contains(metal, "clamp(0.02, 2.0, uniforms.u0)"));
    check(contains(metal, "clamp(0.25, (input.v0).x, uniforms.u0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "lerp(0.5, 1.0, "));
    check(contains(hlsl, "step((input.v0).y, 0.0)"));
    check(contains(hlsl, "clamp(0.02, 2.0, uniforms.u0)"));
};

// Only the screen-space derivatives differ: dfdx/dfdy against HLSL's ddx/ddy.
auto tCodegenTranscendentalNames = test("GPU/codegenTranscendentalNames") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto normal = builder.vertexInput<Float3>();
    auto carried = builder.varying(normal);

    builder.position(float4(position, 0.0f, 1.0f));

    auto angle = atan2(carried.y(), carried.x());
    auto swept = tan(asin(acos(atan(angle))));
    auto falloff = exp(-log(exp2(log2(swept))) * rsqrt(sign(swept) + 2.0f));
    auto edged = ceil(trunc(round(falloff))) + fwidth(falloff) + dfdx(falloff)
                 + dfdy(falloff);
    auto bounced = reflect(carried, normalize(carried))
                   + refract(carried, normalize(carried), 0.5f)
                   + faceforward(carried, carried, carried);

    builder.fragment(float4(bounced * edged, distance(carried, bounced)));

    auto metal = emitMetal(builder.graph());

    for (const auto* name:
         {"atan2(",   "tan(",     "asin(",        "acos(",   "exp(",
          "exp2(",    "log(",     "log2(",        "rsqrt(",  "sign(",
          "ceil(",    "trunc(",   "round(",       "fwidth(", "distance(",
          "reflect(", "refract(", "faceforward(", "dfdx(",   "dfdy("})
        check(contains(metal, name));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "ddx("));
    check(contains(hlsl, "ddy("));
    check(!contains(hlsl, "dfdx("));
    check(!contains(hlsl, "dfdy("));
};

// mod() is recorded as x - y * floor(x / y): the only modulus either backend
// offers is fmod(), which truncates, mirroring every tile left of the origin.
auto tCodegenFlooredModulus = test("GPU/codegenFlooredModulus") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto scale = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(mod(carried, 2.0f), mod(carried.x(), scale), 1.0f));

    auto metal = emitMetal(builder.graph());

    check(contains(metal, "floor("));
    check(!contains(metal, "mod("));

    // Neither half of this commutes, so the operand order is kept.
    check(contains(metal, " - (2.0 * floor("));
    check(contains(metal, " / 2.0)"));
};

// A coordinate swap is one Swizzle node rather than a constructor over rebuilt
// parts, spelled identically by both backends.
auto tCodegenSwizzleOrderings = test("GPU/codegenSwizzleOrderings") = []
{
    // Asserted rather than probed with requires(): an unsatisfied constraint on
    // a plain member function is a hard error at the call, which is the wanted
    // diagnostic, but leaves nothing for a requires-expression to fold to false.
    static_assert(detail::spellableAt(4, "zw"));
    static_assert(detail::spellableAt(2, "yx"));
    static_assert(detail::spellableAt(2, "xxy"));
    static_assert(detail::spellableAt(4, "zyxw"));
    static_assert(!detail::spellableAt(2, "zw"));
    static_assert(!detail::spellableAt(2, "xyz"));
    static_assert(!detail::spellableAt(3, "xyw"));

    static_assert(requires(Float4 value) { value.zw(); });
    static_assert(requires(Float2 value) { value.yx(); });
    static_assert(requires(Float2 value) { value.xxy(); });
    static_assert(requires(Float4 value) { value.zyxw(); });

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float4>();
    auto carried = builder.varying(color);

    builder.position(float4(position.yx(), 0.0f, 1.0f));
    builder.fragment(float4(carried.zyx() + carried.wzy(), carried.zw().y())
                     + carried.zyxw());

    auto metal = emitMetal(builder.graph());
    check(contains(metal, ").yx"));
    check(contains(metal, ").zyx"));
    check(contains(metal, ").wzy"));
    check(contains(metal, ").zw"));
    check(contains(metal, ").zyxw"));

    // One node per swizzle: the source is read once, not rebuilt from four
    // extracted components.
    check(countOccurrences(metal, "input.v0") == 4);

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, ").zyx"));
    check(contains(hlsl, ").zyxw"));
};

// Built from columns, multiplied with * on MSL and mul() on HLSL, and
// transposed at construction on HLSL, which fills a matrix from rows.
auto tCodegenSmallMatrices = test("GPU/codegenSmallMatrices") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto normal = builder.vertexInput<Float3>();
    auto angle = builder.uniform<Float>();

    auto rotation =
        float2x2(float2(cos(angle), sin(angle)), float2(-sin(angle), cos(angle)));

    builder.position(float4(rotation * position, 0.0f, 1.0f));

    auto basis = float3x3(builder.varying(normal),
                          float3(0.0f, 1.0f, builder.constant(0.0f)),
                          float3(0.0f, builder.constant(0.0f), 1.0f));

    builder.fragment(float4(basis * (basis * builder.varying(normal)), 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "float2x2("));
    check(contains(metal, "float3x3("));
    check(!contains(metal, "transpose("));
    check(!contains(metal, "mul("));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "transpose(float2x2("));
    check(contains(hlsl, "transpose(float3x3("));
    check(contains(hlsl, "mul("));
};

// The HLSL check is the one that matters: the construction already emitted a
// transpose of its own, and the two have to nest rather than cancel.
auto tCodegenMatrixTranspose = test("GPU/codegenMatrixTranspose") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto normal = builder.vertexInput<Float3>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto carried = builder.varying(normal);

    // Two matrices rather than one used twice, so neither is promoted to a
    // shared local and each call still has one under it to read.
    auto basis = float3x3(carried,
                          float3(0.0f, 1.0f, builder.constant(0.0f)),
                          float3(0.0f, builder.constant(0.0f), 1.0f));

    auto other = float3x3(float3(1.0f, builder.constant(0.0f), 0.0f),
                          carried,
                          float3(0.0f, builder.constant(0.0f), 1.0f));

    builder.fragment(float4(transpose(basis) * carried, determinant(other)));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "transpose(float3x3("));
    check(contains(metal, "determinant(float3x3("));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "transpose(transpose(float3x3("));
    check(contains(hlsl, "determinant(transpose(float3x3("));
};

auto tCodegenComparisons = test("GPU/codegenComparisons") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto threshold = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto inside = carried.x() < threshold && carried.y() >= 0.0f;
    auto edge = !(carried.x() == threshold);

    builder.fragment(float4(select(inside, 1.0f, 0.0f),
                            select(edge, carried.y(), threshold),
                            select(inside, carried.xy(), carried.yx())));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, " < uniforms.u0)"));
    check(contains(metal, " >= 0.0)"));
    check(contains(metal, " && "));
    check(contains(metal, "(!("));
    check(contains(metal, " == uniforms.u0)"));
    check(contains(metal, " ? 1.0 : 0.0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, " && "));
    check(contains(hlsl, " ? 1.0 : 0.0)"));
};

// A mutable local is a statement, not an expression: declared where created,
// and every read after an assignment sees the assigned value.
auto tCodegenMutableLocal = test("GPU/codegenMutableLocal") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto total = builder.var(0.0f);
    total += carried.x();
    total = total.get() * 2.0f;

    builder.fragment(float4(total, total, total, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "float v0 = 0.0;"));
    check(contains(metal, "v0 = (v0 + (input.v0).x);"));
    check(contains(metal, "v0 = (v0 * 2.0);"));
    check(contains(metal, "return float4(v0, v0, v0, 1.0);"));

    // Statement order is recording order.
    check(metal.find("float v0 = 0.0;") < metal.find("v0 = (v0 + "));
    check(metal.find("v0 = (v0 * 2.0);") < metal.find("return float4(v0"));
};

auto tCodegenBranches = test("GPU/codegenBranches") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto shade = builder.var(0.0f);

    builder.ifThen(
        carried.x() < 0.0f,
        [&] { shade = carried.y(); },
        [&]
        {
            auto inner = builder.var(1.0f);
            inner *= carried.y();
            shade = inner.get();
        });

    builder.fragment(float4(shade, shade, shade, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "if (((input.v0).x < 0.0))"));
    check(contains(metal, "\n    else\n"));

    // The else body's own variable, indented inside the block that owns it.
    check(contains(metal, "\n        float v1 = 1.0;"));
    check(contains(emitHlsl(builder.graph()), "\n        float v1 = 1.0;"));
};

// The condition must be printed into the header rather than bound to a local
// before it, or the loop would test a value that never changes.
auto tCodegenLoop = test("GPU/codegenLoop") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto limit = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto travelled = builder.var(0.0f);
    auto steps = builder.var(0.0f);

    builder.loop(steps < 64.0f,
                 [&]
                 {
                     steps += 1.0f;

                     auto step = abs(carried.x()) + 0.01f;

                     builder.ifThen(step < 0.001f, [&] { builder.breakLoop(); });
                     builder.ifThen(travelled > limit,
                                    [&] { builder.continueLoop(); });

                     travelled += step;
                 });

    builder.fragment(float4(travelled, travelled, travelled, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "while ((v1 < 64.0))"));
    check(contains(metal, "            break;"));
    check(contains(metal, "            continue;"));

    // No bool local is bound ahead of the loop.
    check(metal.find("while (") < metal.find("v1 = (v1 + 1.0);"));
    check(!contains(metal, "bool t"));

    check(contains(emitHlsl(builder.graph()), "while ((v1 < 64.0))"));
};

// A local defined outside the loop would hold the value the first iteration
// computed for every one after it.
auto tCodegenLoopLocalsStayInside = test("GPU/codegenLoopLocalsStayInside") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto total = builder.var(0.0f);

    builder.loop(total < 8.0f,
                 [&]
                 {
                     auto shared = sin(total.get() * carried.x());
                     total += shared * shared;
                 });

    builder.fragment(float4(total, total, total, 1.0f));

    auto metal = emitMetal(builder.graph());

    auto loopAt = metal.find("while (");
    auto localAt = metal.find("float t0 = sin(");

    check(localAt != std::string::npos);
    check(loopAt < localAt);
    check(countOccurrences(metal, "sin(") == 1);
};

// The name spans two statements, the shape every raymarcher has: measure the
// distance, stop if it is small enough, otherwise step by it.
auto tCodegenSharedAcrossStatements = test("GPU/codegenSharedAcrossStatements") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto travelled = builder.var(0.0f);

    builder.loop(travelled < 10.0f,
                 [&]
                 {
                     auto distance =
                         length(float3(carried, 1.0f) * travelled.get()) - 1.0f;

                     builder.ifThen(distance < 0.001f, [&] { builder.breakLoop(); });

                     travelled += distance;
                 });

    builder.fragment(float4(travelled, travelled, travelled, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(countOccurrences(metal, "length(") == 1);
    check(contains(metal, "if ((t0 < 0.001))"));
    check(contains(metal, "v0 = (v0 + t0);"));
};

// A name is given up the moment a statement writes a variable the value behind
// it was computed from.
auto tCodegenStaleLocalsAreDropped = test("GPU/codegenStaleLocalsAreDropped") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto total = builder.var(carried.x());
    auto shade = builder.var(0.0f);

    auto scaled = sin(total.get());

    shade = scaled + scaled;
    total = total.get() + 1.0f;
    shade = scaled * 2.0f + scaled;

    builder.fragment(float4(shade, shade, shade, 1.0f));

    auto metal = emitMetal(builder.graph());

    // What stands for sin(v0) before v0 moves cannot stand for it afterwards.
    check(countOccurrences(metal, "sin(v0)") == 2);
    check(contains(metal, "float t0 = sin(v0);"));
    check(contains(metal, "float t1 = sin(v0);"));
    check(metal.find("v0 = (v0 + 1.0);") < metal.find("float t1 = sin(v0);"));
};

// GLSL has transpose, determinant and inverse; MSL and HLSL have the first two
// and neither has the third, which is why only two are here.
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

// MSL's operator and HLSL's mul() both read whichever operand is on the left as
// a row vector, so the written order is what tells the products apart - and the
// other order is a different value that compiles just as happily.
auto tCodegenVectorTimesMatrix = test("GPU/codegenVectorTimesMatrix") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto angle = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto carried = builder.varying(position);

    // Two matrices rather than one used twice, so neither is promoted to a
    // shared local and each product still has one under it.
    auto into =
        float2x2(float2(cos(angle), sin(angle)), float2(-sin(angle), cos(angle)));

    auto back =
        float2x2(float2(cos(angle), -sin(angle)), float2(sin(angle), cos(angle)));

    builder.fragment(float4(into * carried, carried * back));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "(float2x2("));
    check(contains(metal, " * float2x2("));

    // On HLSL the product is a call, so the matrix is mul()'s first argument in
    // one and its second in the other.
    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "mul(transpose(float2x2("));
    check(contains(hlsl, ", transpose(float2x2("));
};

// Only the compiler answers whether the languages take a literal where the
// emitter put one, and a vector on the left of a product.
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

// Emitted text says the statements are there; only the compiler says the
// language will take them.
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

// An intrinsic a backend spells differently, or a swizzle it will not take,
// only shows up when the platform compiler reads the source.
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

// The HLSL cbuffer is a global both stages already see, so only the MSL
// signatures move.
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

// One block, bound twice, one slot rule.
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

// Each flag is checked against whether the Metal signature beside it declared
// the block, so a bound stage and a declared parameter cannot drift apart.
auto tCodegenUniformStages = test("GPU/codegenUniformStages") = []
{
    // build() emits the host's backend, so the signature is read through
    // emitMetal for a platform-independent comparison.
    auto stagesOf = [](const ShaderGraph& graph, const GeneratedShader& generated)
    {
        auto metal = emitMetal(graph);
        auto declaration = uniformDecl(RenderPass::uniformBase);

        auto vertexDeclares = contains(
            metal, "vertexMain(VertexIn input [[stage_in]], " + declaration);
        auto fragmentDeclares = contains(
            metal, "fragmentMain(VertexOut input [[stage_in]],\n    " + declaration);

        // Two statements of one fact: assert they agree before reading either.
        check(generated.vertexReadsUniforms == vertexDeclares);
        check(generated.fragmentReadsUniforms == fragmentDeclares);
    };

    auto vertexOnly = ShaderBuilder {};
    auto vertexPosition = vertexOnly.vertexInput<Float2>();
    auto scale = vertexOnly.uniform<Float>();
    vertexOnly.position(
        float4(vertexPosition.x() * scale, vertexPosition.y() * scale, 0.0f, 1.0f));
    vertexOnly.fragment(float4(vertexOnly.constant(1.0f),
                               vertexOnly.constant(1.0f),
                               vertexOnly.constant(1.0f),
                               vertexOnly.constant(1.0f)));

    auto vertexShader = vertexOnly.build();
    stagesOf(vertexOnly.graph(), vertexShader);
    check(vertexShader.vertexReadsUniforms);
    check(!vertexShader.fragmentReadsUniforms);

    auto fragmentOnly = ShaderBuilder {};
    auto fragmentPosition = fragmentOnly.vertexInput<Float2>();
    auto color = fragmentOnly.uniform<Float4>();
    fragmentOnly.position(float4(fragmentPosition, 0.0f, 1.0f));
    fragmentOnly.fragment(color);

    auto fragmentShader = fragmentOnly.build();
    stagesOf(fragmentOnly.graph(), fragmentShader);
    check(!fragmentShader.vertexReadsUniforms);
    check(fragmentShader.fragmentReadsUniforms);

    // Read by neither stage, though the program still has a block to pack.
    auto unread = ShaderBuilder {};
    auto unreadPosition = unread.vertexInput<Float2>();
    auto unusedTint = unread.uniform<Float4>();
    (void) unusedTint;
    unread.position(float4(unreadPosition, 0.0f, 1.0f));
    unread.fragment(float4(unread.constant(1.0f),
                           unread.constant(0.0f),
                           unread.constant(0.0f),
                           unread.constant(1.0f)));

    auto unreadShader = unread.build();
    stagesOf(unread.graph(), unreadShader);
    check(!unreadShader.vertexReadsUniforms);
    check(!unreadShader.fragmentReadsUniforms);
};

// The flag is collected over the statement roots, not the colour expression
// alone: otherwise a uniform read only inside a loop would go unbound.
auto tCodegenUniformInStatementBinds =
    test("GPU/codegenUniformInStatementBinds") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);
    auto threshold = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto shade = builder.var(0.0f);
    builder.ifThen(carried.x() > threshold, [&] { shade = threshold; });

    builder.fragment(float4(shade, shade, shade, 1.0f));

    auto generated = builder.build();
    check(!generated.vertexReadsUniforms);
    check(generated.fragmentReadsUniforms);
    check(contains(emitMetal(builder.graph()),
                   "fragmentMain(VertexOut input [[stage_in]],\n    "
                       + uniformDecl(RenderPass::uniformBase)));
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

// Paired texture/sampler bindings at the same index: fragment parameters on
// Metal, globals with t/s registers on D3D.
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

    // The vertex stage carries no texture parameters.
    check(
        contains(metal, "vertex VertexOut vertexMain(VertexIn input [[stage_in]])"));
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

// Metal passes the mip level to the same sample(), HLSL has a method of its
// own. One graph node, so a shader says it once.
auto tCodegenSampleLevelEmits = test("GPU/codegenSampleLevelEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);
    auto level = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, varyingUv, level));

    auto metal = emitMetal(builder.graph());
    check(
        contains(metal, "texture0.sample(sampler0, input.v0, level(uniforms.u0))"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "texture0.SampleLevel(sampler0, input.v0, uniforms.u0)"));
};

// A plain float needs no anchoring: the texture already carries the graph the
// constant records into.
auto tCodegenLiteralSampleLevel = test("GPU/codegenLiteralSampleLevel") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, varyingUv, 0.0f));

    check(contains(emitMetal(builder.graph()),
                   "texture0.sample(sampler0, input.v0, level(0.0))"));

    check(contains(emitHlsl(builder.graph()),
                   "texture0.SampleLevel(sampler0, input.v0, 0.0)"));
};

// The coordinate goes through int2 on both backends: Metal reads unsigned, so
// a negative coordinate must become a large one rather than an undefined
// conversion, which is what makes it read as zero on both.
auto tCodegenFetchEmits = test("GPU/codegenFetchEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(fetch(image, varyingUv));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "texture0.read(uint2(int2(input.v0)))"));
    check(!contains(metal, "texture0.sample"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "texture0.Load(int3(int2(input.v0), 0))"));
    check(!contains(hlsl, "texture0.Sample"));
};

// An unsampled level and a texel read are each one method call the backend
// either has or does not.
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

// The kernel scaffolding differs per backend (function parameters on Metal,
// globals + numthreads on D3D); the body and the count guard are shared.
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

// A kernel without user uniforms still gets the block: the implicit count
// lives there.
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

// The literal forms record uint constant nodes, so the whole loop header spells
// in uints.
auto tCodegenComputeUIntLoop = test("GPU/codegenComputeUIntLoop") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto count = builder.uniform<UInt>();
    auto gid = builder.threadId();

    auto total = builder.var(0.0f);
    auto i = builder.var(0u);

    builder.loop(i < count,
                 [&]
                 {
                     total += input[gid * count + i];
                     i += 1u;
                 });

    builder.ifThen(gid == 0u, [&] { total *= 2.0f; });

    builder.write(output, gid, total);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "uint v1 = 0u;"));
        check(contains(source, "while ((v1 < uniforms.u0))"));
        check(contains(source, "v1 = (v1 + 1u);"));
        check(contains(source, "if ((gid == 0u))"));

        // The condition reads the counter the body advances, so it must not be
        // bound to a local before the loop.
        check(source.find("while (") < source.find("v1 = (v1 + 1u);"));
    }
};

// Into the signed vocabulary for arithmetic that may go below zero, back out
// with toUInt once clamped. Constructor-style casts on both backends.
auto tCodegenComputeIndexCasts = test("GPU/codegenComputeIndexCasts") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto scale = builder.uniform<Float>();
    auto gid = builder.threadId();

    auto previous = input[toUInt(max(toInt(gid) - 1, 0))];
    auto scaled = input[toUInt(toFloat(gid) * scale)];

    builder.write(output, gid, previous + scaled);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "uint(max((int(gid) - 1), 0))"));
        check(contains(source, "uint((float(gid) * uniforms.u0))"));
    }
};

// A rank that reached the dispatch but not the emitter would leave the kernel
// reading a thread id of the wrong shape and say nothing about it.
auto tCodegenCompute2D = test("GPU/codegenCompute2D") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto position = builder.threadPosition();

    builder.write(output, position.y * 16u + position.x, toFloat(position.x));

    auto shader = builder.build();
    check(shader.dispatchRank == DispatchRank::TwoD);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint2 gid [[thread_position_in_grid]]"));
    check(contains(metal, "uint width;"));
    check(contains(metal, "uint height;"));
    check(!contains(metal, "uint count;"));
    check(
        contains(metal, "if (gid.x >= uniforms.width || gid.y >= uniforms.height)"));
    check(contains(metal, "buffer0[((gid.y * 16u) + gid.x)] = float(gid.x);"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "[numthreads(8, 8, 1)]"));
    check(contains(hlsl, "uint3 threadId : SV_DispatchThreadID"));
    check(contains(hlsl, "uint2 gid = threadId.xy;"));
    check(
        contains(hlsl, "if (gid.x >= uniforms.width || gid.y >= uniforms.height)"));
    check(contains(hlsl, "buffer0[((gid.y * 16u) + gid.x)] = float(gid.x);"));
};

// Read and written textures take slots from one counter, because Metal binds
// both to one texture index space; on D3D they land in the t and u spaces they
// share with the storage buffers, above every buffer slot.
auto tCodegenComputeTextureWrite = test("GPU/codegenComputeTextureWrite") = []
{
    auto builder = ShaderBuilder {};

    auto source = builder.texture();
    auto target = builder.writableTexture();
    auto p = builder.threadPosition();

    builder.write(
        target, p.x, p.y, sample(source, float2(toFloat(p.x), toFloat(p.y))));

    auto shader = builder.build();
    check(shader.source.isCompute());
    check(shader.source.computeEntry == "computeMain");

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "texture2d<float> texture0 [[texture(0)]]"));
    check(contains(metal, "sampler sampler0 [[sampler(0)]]"));
    check(
        contains(metal, "texture2d<float, access::write> texture1 [[texture(1)]]"));

    // A written texture has no sampler on either backend.
    check(!contains(metal, "sampler sampler1"));
    check(contains(metal,
                   "texture1.write(texture0.sample(sampler0, float2(float(gid.x), "
                   "float(gid.y))), uint2(gid.x, gid.y));"));

    // Built from ComputePass::textureRegisterBase rather than a literal: a test
    // naming a number would only say where the base used to be.
    auto textureRegister = [](int slot)
    { return std::to_string(ComputePass::textureRegisterBase + slot); };

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "Texture2D texture0 : register(t" + textureRegister(0) + ");"));
    check(contains(hlsl, "SamplerState sampler0 : register(s0);"));
    check(contains(hlsl,
                   "RWTexture2D<float4> texture1 : register(u" + textureRegister(1)
                       + ");"));
    check(!contains(hlsl, "SamplerState sampler1"));
    check(contains(hlsl,
                   "texture1[uint2(gid.x, gid.y)] = texture0.Sample(sampler0, "
                   "float2(float(gid.x), float(gid.y)));"));
};

// Recording any store is what marks a graph as a kernel, and a graph with no
// storage buffer emits none.
auto tCodegenComputeTextureOnly = test("GPU/codegenComputeTextureOnly") = []
{
    auto builder = ShaderBuilder {};

    auto target = builder.writableTexture();
    auto p = builder.threadPosition();
    auto shade = toFloat(p.x) * 0.25f;

    builder.write(target, p.x, p.y, float4(shade, shade, shade, 1.0f));

    auto shader = builder.build();
    check(shader.source.isCompute());

    for (const auto& text: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(!contains(text, "buffer0"));
        check(contains(text, "uniforms.width"));
        check(contains(text, "float t0 = (float(gid.x) * 0.25);"));
    }
};

// The rank is a property of what the body asked for, not a new default.
auto tCodegenCompute1DUnchanged = test("GPU/codegenCompute1DKeepsScalarId") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto gid = builder.threadId();

    builder.write(output, gid, toFloat(gid));

    auto shader = builder.build();
    check(shader.dispatchRank == DispatchRank::OneD);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint gid [[thread_position_in_grid]]"));
    check(!contains(metal, "uint2 gid"));
    check(contains(metal, "if (gid >= uniforms.count)"));
};

// read4(i) is elements 4i..4i+3, so a kernel over a struct of four floats never
// spells the stride. The buffer is still a run of floats underneath, which is
// what keeps its bytes bindable as a per-instance stream.
auto tCodegenComputeVectorElements = test("GPU/codegenComputeVectorElements") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    auto record = input.read4(i);
    builder.write(output, i, record * 2.0f);

    for (const auto& text: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        // The read and the write build the base index through separate calls,
        // but it is the same pure expression, so the graph hands both one node.
        check(contains(text, "uint t0 = (gid * 4u);"));
        check(countOccurrences(text, "(gid * 4u)") == 1);

        // Each offset off that base is addressed by the read and by the write,
        // so each is named once.
        check(contains(text, "uint t1 = (t0 + 1u);"));
        check(contains(text, "uint t2 = (t0 + 2u);"));
        check(contains(text, "uint t3 = (t0 + 3u);"));

        check(contains(text,
                       "float4 t4 = (float4(buffer0[t0], buffer0[t1], "
                       "buffer0[t2], buffer0[t3]) * 2.0);"));

        check(contains(text, "buffer1[t0] = (t4).x;"));
        check(contains(text, "buffer1[t1] = (t4).y;"));
        check(contains(text, "buffer1[t2] = (t4).z;"));
        check(contains(text, "buffer1[t3] = (t4).w;"));
    }
};

// read2 strides by two and read3 by three, so a buffer of pairs and one of
// triples each index in their own units.
auto tCodegenComputeVectorStrides = test("GPU/codegenComputeVectorStrides") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, input.read2(i));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint t0 = (gid * 2u);"));
    check(contains(metal, "float2 t2 = float2(buffer0[t0], buffer0[t1]);"));
    check(contains(metal, "buffer1[t1] = (t2).y;"));
    check(!contains(metal, "t0 + 2u"));

    auto triples = ShaderBuilder {};
    auto source = triples.inputBuffer();
    auto index = triples.threadId();
    triples.write(triples.outputBuffer(), index, source.read3(index));

    auto hlsl = emitHlsl(triples.graph());
    check(contains(hlsl, "uint t0 = (gid * 3u);"));
    check(contains(hlsl,
                   "float3 t3 = float3(buffer0[t0], buffer0[t1], buffer0[t2]);"));
    check(contains(hlsl, "buffer1[t2] = (t3).z;"));
    check(!contains(hlsl, "t0 + 3u"));
};

// The same InputBuffer a kernel subscripts, in a graph with no stores, so the
// shader is a vertex/fragment pair. What it buys is the indexed read a vertex
// attribute cannot do: the shader picks the element.
auto tCodegenFragmentBufferRead = test("GPU/codegenFragmentBufferRead") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto palette = builder.inputBuffer();
    auto record = builder.uniform<UInt>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(palette.read3(record), 1.0f));

    auto metal = emitMetal(builder.graph());

    // Declared on the stage that reads it and nowhere else: a buffer parameter
    // on the vertex function would be dead weight the caller still has to bind.
    check(contains(metal,
                   "device const float* buffer0 [[buffer("
                       + std::to_string(RenderPass::bufferBase) + ")]]"));
    check(countOccurrences(metal, "device const float* buffer0") == 1);
    check(metal.find("fragmentMain") < metal.find("device const float* buffer0"));

    // Read-only in a render stage, whatever a kernel would have got.
    check(!contains(metal, "device float* buffer0"));

    // An HLSL global is visible to both functions, so it is declared once
    // outside either, above every texture register.
    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "StructuredBuffer<float> buffer0 : register(t"
                       + std::to_string(RenderPass::bufferRegisterBase) + ");"));
    check(!contains(hlsl, "RWStructuredBuffer"));

    // read3 strides by three, so an index the shader computed addresses its own
    // record.
    for (const auto& source: {metal, hlsl})
    {
        check(contains(source, "uint t0 = (uniforms.u0 * 3u);"));
        check(contains(source,
                       "float3(buffer0[t0], buffer0[(t0 + 1u)], "
                       "buffer0[(t0 + 2u)])"));
    }
};

// Nothing about the binding is fragment-specific, which is what lets a vertex
// shader place a record it looked up rather than one it was handed.
auto tCodegenVertexBufferRead = test("GPU/codegenVertexBufferRead") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto offsets = builder.inputBuffer();
    auto record = builder.uniform<UInt>();

    builder.position(float4(position + offsets.read2(record), 0.0f, 1.0f));
    builder.fragment(float4(builder.constant(1.0f), 1.0f, 1.0f, 1.0f));

    auto metal = emitMetal(builder.graph());

    check(contains(metal,
                   "device const float* buffer0 [[buffer("
                       + std::to_string(RenderPass::bufferBase) + ")]]"));
    check(countOccurrences(metal, "device const float* buffer0") == 1);
    check(metal.find("device const float* buffer0") < metal.find("fragmentMain"));
};

auto tCodegenBothStagesReadBuffer = test("GPU/codegenBothStagesReadBuffer") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto data = builder.inputBuffer();
    auto record = builder.uniform<UInt>();

    builder.position(float4(position * data[record], 0.0f, 1.0f));
    builder.fragment(float4(data[record + 1u], 0.0f, 0.0f, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(countOccurrences(metal, "device const float* buffer0") == 2);

    auto hlsl = emitHlsl(builder.graph());
    check(countOccurrences(hlsl, "StructuredBuffer<float> buffer0") == 1);
};

// Only the compiler says the registers and buffer indices the emitter picked
// are ones the backend accepts.
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

// All of this spells identically in MSL and HLSL, so both backends are checked
// against the same text.
auto tCodegenIntegerOperators = test("GPU/codegenIntegerOperators") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto index = toInt(carried.x() * 4.0f) & 3;
    auto scrambled = ((index << 2) | (index >> 1)) ^ ~index;
    auto shade = toFloat(scrambled % 5) * 0.2f;

    builder.fragment(float4(shade, shade, shade, 1.0f));

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        // An int literal carries no suffix, unlike a uint's.
        check(contains(source, "int t0 = (int(((input.v0).x * 4.0)) & 3);"));

        // The shifts are the only operators that do not fit in the char the
        // graph carries an operator in.
        check(contains(source, "(t0 << 2)"));
        check(contains(source, "(t0 >> 1)"));

        check(contains(source, "(~(t0))"));
        check(contains(source, "% 5)"));
        check(contains(source, "float("));
    }
};

// MSL and HLSL both give a signed integer four bytes and pack it where they
// pack a float, so the block needs no padding to reconcile them.
auto tCodegenIntegerUniform = test("GPU/codegenIntegerUniform") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto first = builder.uniform<Int>();
    auto scale = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto shade = toFloat(first + 1) * scale * carried.x();
    builder.fragment(float4(shade, shade, shade, 1.0f));

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "int u0;"));
        check(contains(source, "float u1;"));
        check(contains(source, "float((uniforms.u0 + 1))"));

        // Two four-byte scalars in a row: the rule sets agree on where the
        // second lands, so nothing is padded between them.
        check(!contains(source, "pad"));
    }

    auto types = Vector<ValueType> {};
    types.add(ValueType::Int);
    types.add(ValueType::Float);

    auto offsets = uniformOffsets(types);
    check(offsets[0] == 0);
    check(offsets[1] == 4);
};

// Declared once at the top of the stage that subscripts it, and nowhere else.
auto tCodegenConstantArray = test("GPU/codegenConstantArray") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto palette = builder.array(float3(builder.constant(0.1f), 0.1f, 0.2f),
                                 float3(builder.constant(0.9f), 0.4f, 0.2f),
                                 float3(builder.constant(0.2f), 0.8f, 0.6f),
                                 float3(builder.constant(1.0f), 0.9f, 0.7f));

    auto index = toInt(carried.x() * 4.0f) & 3;
    auto picked = palette[index];

    builder.fragment(float4(picked * 0.5f + picked * 0.5f + palette[0], 1.0f));

    auto declaration = std::string {
        "const float3 a0[4] = {float3(0.1, 0.1, 0.2), float3(0.9, 0.4, 0.2), "
        "float3(0.2, 0.8, 0.6), float3(1.0, 0.9, 0.7)};"};

    auto read =
        std::string {"float3 t0 = (a0[(int(((input.v0).x * 4.0)) & 3)] * 0.5);"};

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(countOccurrences(source, declaration) == 1);

        // The subscript and the scale above it are one pure expression written
        // twice, so they collapse to a single name.
        check(countOccurrences(source, read) == 1);
        check(countOccurrences(source, "t0") == 3);

        check(contains(source, "a0[0]"));
        check(source.find(declaration) < source.find(read));

        check(source.find("fragmentMain") < source.find(declaration));
    }
};

// The emitted text says the vocabulary is there; only the compiler says the
// language will take it.
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

    // A signed index a negative coordinate really does make negative, held in
    // range two different ways: the mask, and the clamp.
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

// Both languages spell the type and every operation on it the same way, so
// both backends are checked against the same text.
auto tCodegenIntegerVectors = test("GPU/codegenIntegerVectors") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto cell = toInt(carried * 16.0f);

    auto wrapped = (cell & 7) + int2(cell.y(), cell.x());
    auto shifted = wrapped << 1;

    auto shade = toFloat(shifted.x() + shifted.y()) * 0.01f;
    auto tint = toFloat(-cell) * 0.001f;

    builder.fragment(float4(shade + tint.x(), shade, shade, 1.0f));

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        // One cast for the whole vector, which keeps the coordinate behind it
        // recorded once.
        check(contains(source, "int2 t0 = int2((input.v0 * 16.0));"));

        check(
            contains(source, "int2 t1 = (((t0 & 7) + int2((t0).y, (t0).x)) << 1);"));

        // A component of an integer vector is an integer, so the crossing back
        // into float arithmetic is still spelled out.
        check(contains(source, "float(((t1).x + (t1).y))"));
        check(contains(source, "float2((-(t0)))"));
    }
};

// `<` on two vectors is a mask in both languages, and all()/any() is the only
// thing that turns one into a condition a branch or a select can take.
auto tCodegenVectorComparison = test("GPU/codegenVectorComparison") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto limit = builder.uniform<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto inside = carried < limit;
    auto outside = !inside;

    auto lit = select(all(inside), 1.0f, 0.25f);
    auto edge = select(any(outside), 0.5f, 0.0f);

    builder.fragment(float4(lit, edge, lit, 1.0f));

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        // The result is a mask of the operands' width rather than a scalar.
        check(contains(source, "bool2 t0 = (input.v0 < uniforms.u0);"));

        // The negation is the operator too, which is what GLSL spells not().
        check(contains(source, "any((!(t0)))"));
        check(contains(source, "all(t0) ?"));
    }
};

// An Int2 crosses from the CPU where a Bool2 does not, and packs exactly where
// a Float2 does, so the block needs no padding to reconcile the two backends.
auto tCodegenIntegerVectorUniform = test("GPU/codegenIntegerVectorUniform") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto origin = builder.uniform<Int2>();
    auto scale = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto cell = toInt(carried * 16.0f) - origin;
    auto shade = toFloat(cell.x() + cell.y()) * scale;

    builder.fragment(float4(shade, shade, shade, 1.0f));

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "int2 u0;"));
        check(contains(source, "float u1;"));
        check(contains(source, "- uniforms.u0)"));

        // The rule sets agree on where the four-byte value after the eight-byte
        // one lands, so nothing is padded between them.
        check(!contains(source, "pad"));
    }

    auto types = Vector<ValueType> {};
    types.add(ValueType::Int2);
    types.add(ValueType::Float);

    auto offsets = uniformOffsets(types);
    check(offsets[0] == 0);
    check(offsets[1] == 8);
};

// The emitted text says the vocabulary is there; only the compiler says the
// language will take it.
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

// A kernel that barriers gets no early return, since a barrier below a return
// some threads took is undefined on both backends; gridCount() bounds its loads
// instead.
auto tCodegenComputeSharedReduction = test("GPU/codegenComputeSharedReduction") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto gid = builder.threadId();
    auto lid = builder.localId();
    auto group = builder.groupId();
    auto tile = builder.shared<Float>(64);

    auto value = builder.var(0.0f);
    builder.ifThen(gid < builder.gridCount(), [&] { value = input[gid]; });
    builder.write(tile, lid, value.get());
    builder.barrier();

    builder.ifThen(lid < 32u,
                   [&] { builder.write(tile, lid, tile[lid] + tile[lid + 32u]); });
    builder.barrier();

    builder.ifThen(lid == 0u, [&] { builder.write(output, group, tile[0u]); });

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint lid [[thread_position_in_threadgroup]]"));
    check(contains(metal, "uint tgid [[threadgroup_position_in_grid]]"));
    check(contains(metal, "threadgroup float s0[64];"));
    check(contains(metal, "threadgroup_barrier(mem_flags::mem_threadgroup);"));
    check(!contains(metal, "return;"));
    check(contains(metal, "if ((gid < uniforms.count))"));
    check(contains(metal, "s0[lid] = v0;"));
    check(contains(metal, "s0[lid] = (s0[lid] + s0[(lid + 32u)]);"));
    check(contains(metal, "buffer1[tgid] = s0[0u];"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "groupshared float s0[64];"));
    check(contains(hlsl, "uint3 localThread : SV_GroupThreadID"));
    check(contains(hlsl, "uint3 groupIndex : SV_GroupID"));
    check(contains(hlsl, "uint lid = localThread.x;"));
    check(contains(hlsl, "uint tgid = groupIndex.x;"));
    check(contains(hlsl, "GroupMemoryBarrierWithGroupSync();"));
    check(!contains(hlsl, "return;"));
    check(contains(hlsl, "s0[lid] = (s0[lid] + s0[(lid + 32u)]);"));
    check(contains(hlsl, "buffer1[tgid] = s0[0u];"));
};

// What the tile held before other threads' stores were published is not what it
// holds after, so the emitter re-reads rather than reusing the local.
auto tCodegenComputeSharedNamesRetire =
    test("GPU/codegenComputeSharedNamesRetire") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto gid = builder.threadId();
    auto lid = builder.localId();
    auto tile = builder.shared<Float>(64);

    builder.write(tile, lid, toFloat(gid));
    builder.barrier();

    // Used twice, so it takes a name.
    auto sum = tile[lid] + 1.0f;
    builder.write(output, gid, sum * sum);

    builder.barrier();

    // The pre-barrier name is gone, so the element is read - and named - afresh.
    builder.write(output, gid + 1u, sum * sum);

    auto metal = emitMetal(builder.graph());
    check(countOccurrences(metal, "s0[lid] + 1.0") == 2);
    check(contains(metal, "float t0 = (s0[lid] + 1.0);"));
    check(contains(metal, "float t1 = (s0[lid] + 1.0);"));
    check(contains(metal, "buffer0[gid] = (t0 * t0);"));
    check(contains(metal, "buffer0[(gid + 1u)] = (t1 * t1);"));
};

// A shared array of float4 declares its element type verbatim: it never crosses
// the CPU boundary, so there is no scalar-layout contract to decompose it into.
auto tCodegenComputeShared2DFloat4 = test("GPU/codegenComputeShared2DFloat4") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto position = builder.threadPosition();
    auto local = builder.localPosition();
    auto group = builder.groupPosition();
    auto tile = builder.shared<Float4>(64);

    auto flatLocal = local.y * 8u + local.x;
    builder.write(
        tile, flatLocal, input.read4(position.y * builder.gridWidth() + position.x));
    builder.barrier();

    auto picked = tile[group.x % 8u + group.y];
    builder.write(output, position.y * builder.gridWidth() + position.x, picked);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint2 lid [[thread_position_in_threadgroup]]"));
    check(contains(metal, "uint2 tgid [[threadgroup_position_in_grid]]"));
    check(contains(metal, "threadgroup float4 s0[64];"));
    check(contains(metal, "s0[((lid.y * 8u) + lid.x)] = "));
    check(contains(metal, "uniforms.width"));
    check(!contains(metal, "return;"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "groupshared float4 s0[64];"));
    check(contains(hlsl, "uint2 lid = localThread.xy;"));
    check(contains(hlsl, "uint2 tgid = groupIndex.xy;"));
};
