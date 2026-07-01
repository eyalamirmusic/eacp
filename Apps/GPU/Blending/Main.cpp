#include <eacp/Graphics/Graphics.h>
#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

namespace
{
constexpr int viewWidth = 900;
constexpr int viewHeight = 300;

// Three RGB translucent quads that overlap in a triangle arrangement, so each
// mode picks a distinctive look:
//   None       - last-drawn quad wins wherever they overlap; the earlier ones
//                are simply overwritten.
//   AlphaBlend - the classic translucency stack: overlaps darken slightly and
//                mix towards grey.
//   Additive   - overlaps brighten: R+G reads yellow, R+B magenta, all three
//                white.
// All in a small local space centred on x=0, translated into each panel via a
// per-draw offset uniform.
struct Vertex
{
    float position[2];
    float color[4];
};

constexpr float quadHalf = 0.25f;
constexpr float alpha = 0.6f;

constexpr Vertex quads[] = {
    // Red quad, top-left.
    {{-0.30f - quadHalf, +0.15f - quadHalf}, {1.f, 0.f, 0.f, alpha}},
    {{-0.30f + quadHalf, +0.15f - quadHalf}, {1.f, 0.f, 0.f, alpha}},
    {{-0.30f - quadHalf, +0.15f + quadHalf}, {1.f, 0.f, 0.f, alpha}},
    {{-0.30f + quadHalf, +0.15f - quadHalf}, {1.f, 0.f, 0.f, alpha}},
    {{-0.30f + quadHalf, +0.15f + quadHalf}, {1.f, 0.f, 0.f, alpha}},
    {{-0.30f - quadHalf, +0.15f + quadHalf}, {1.f, 0.f, 0.f, alpha}},

    // Green quad, top-right.
    {{+0.30f - quadHalf, +0.15f - quadHalf}, {0.f, 1.f, 0.f, alpha}},
    {{+0.30f + quadHalf, +0.15f - quadHalf}, {0.f, 1.f, 0.f, alpha}},
    {{+0.30f - quadHalf, +0.15f + quadHalf}, {0.f, 1.f, 0.f, alpha}},
    {{+0.30f + quadHalf, +0.15f - quadHalf}, {0.f, 1.f, 0.f, alpha}},
    {{+0.30f + quadHalf, +0.15f + quadHalf}, {0.f, 1.f, 0.f, alpha}},
    {{+0.30f - quadHalf, +0.15f + quadHalf}, {0.f, 1.f, 0.f, alpha}},

    // Blue quad, bottom-centre.
    {{+0.00f - quadHalf, -0.25f - quadHalf}, {0.f, 0.f, 1.f, alpha}},
    {{+0.00f + quadHalf, -0.25f - quadHalf}, {0.f, 0.f, 1.f, alpha}},
    {{+0.00f - quadHalf, -0.25f + quadHalf}, {0.f, 0.f, 1.f, alpha}},
    {{+0.00f + quadHalf, -0.25f - quadHalf}, {0.f, 0.f, 1.f, alpha}},
    {{+0.00f + quadHalf, -0.25f + quadHalf}, {0.f, 0.f, 1.f, alpha}},
    {{+0.00f - quadHalf, -0.25f + quadHalf}, {0.f, 0.f, 1.f, alpha}},
};

constexpr int vertexCount = (int) (sizeof(quads) / sizeof(quads[0]));

// A single uniform: the horizontal offset that places the shared geometry into
// one of three columns. NDC panel centres at -2/3, 0, +2/3.
struct Uniforms
{
    float panelOffsetX;
};

// Trivial passthrough that translates by the uniform. Fragment just emits the
// vertex colour, alpha and all - the pipeline's blend mode does the rest.
GeneratedShader makeBlendingShader()
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float4>();
    auto panelOffsetX = builder.uniform<Float>();
    auto varyingColor = builder.varying(color);

    builder.position(
        float4(position.x() + panelOffsetX, position.y(), 0.0f, 1.0f));
    builder.fragment(varyingColor);
    return builder.build();
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = viewWidth;
    options.height = viewHeight;
    options.title = "eacp GPU - Blending";
    return options;
}
} // namespace

// Three panels, one per BlendMode, sharing one vertex buffer and one shader.
// The visual difference is the whole point of the example: identical geometry,
// identical fragment output, three different composition rules.
struct BlendingView final : GPUView
{
    BlendingView()
        : shader(makeBlendingShader())
        , vertexBuffer(Device::shared().makeBuffer(quads))
        , library(Device::shared().makeShaderLibrary(shader.source))
        , none(makePipeline(BlendMode::None))
        , alphaBlend(makePipeline(BlendMode::AlphaBlend))
        , additive(makePipeline(BlendMode::Additive))
    {
    }

    RenderPipeline makePipeline(BlendMode mode)
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout = shader.vertexLayout;
        descriptor.blendMode = mode;
        return Device::shared().makeRenderPipeline(descriptor);
    }

    void drawPanel(RenderPass& pass,
                   const RenderPipeline& pipeline,
                   float panelOffsetX)
    {
        auto uniforms = Uniforms {panelOffsetX};
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.setVertexBytes(&uniforms, sizeof(uniforms), 0);
        pass.draw(vertexCount);
    }

    void render(Frame& frame) override
    {
        // Neutral grey clear so alpha-blend and additive read distinctly
        // against it - a black clear would hide the additive glow, a white
        // clear would swallow the alpha blend.
        auto pass = frame.beginPass({Graphics::Color {0.20f, 0.20f, 0.22f}});
        drawPanel(pass, none, -2.0f / 3.0f);
        drawPanel(pass, alphaBlend, 0.0f);
        drawPanel(pass, additive, +2.0f / 3.0f);
    }

    GeneratedShader shader;
    Buffer vertexBuffer;
    ShaderLibrary library;
    RenderPipeline none;
    RenderPipeline alphaBlend;
    RenderPipeline additive;
};

struct BlendingApp
{
    BlendingApp() { window.setContentView(view); }

    BlendingView view;
    Graphics::Window window {windowOptions()};
};

int main()
{
    eacp::Apps::run<BlendingApp>();
    return 0;
}
