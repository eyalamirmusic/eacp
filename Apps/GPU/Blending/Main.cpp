#include <eacp/GPU/GPU.h>

#include <vector>

using namespace eacp;
using namespace GPU;

namespace
{
constexpr int viewWidth = 900;
constexpr int viewHeight = 360;
constexpr float pi = 3.14159265358979323846f;

constexpr float circleRadius = 0.20f;
constexpr float circleAlpha = 0.75f;
constexpr int circleSegments = 64;
constexpr float centerDistance = 0.15f;

struct Vertex
{
    float position[2];
    float color[4];
};

struct Uniforms
{
    float panelOffsetX;
};

struct CircleSpec
{
    float centerX;
    float centerY;
    float r;
    float g;
    float b;
};

// Precomputed so `circles` stays constexpr and dodges runtime static init.
constexpr float sin120 = 0.8660254f;
constexpr float cos120 = -0.5f;

constexpr CircleSpec circles[] = {
    {0.f, +centerDistance, 1.f, 0.f, 0.f},
    {-centerDistance * sin120, +centerDistance * cos120, 0.f, 1.f, 0.f},
    {+centerDistance * sin120, +centerDistance * cos120, 0.f, 0.f, 1.f},
};

void appendCircle(std::vector<Vertex>& out, const CircleSpec& c)
{
    for (auto i = 0; i < circleSegments; ++i)
    {
        const auto a0 = (float) i / (float) circleSegments * 2.f * pi;
        const auto a1 = (float) (i + 1) / (float) circleSegments * 2.f * pi;

        const Vertex centre {{c.centerX, c.centerY}, {c.r, c.g, c.b, circleAlpha}};
        const Vertex rim0 {{c.centerX + circleRadius * std::cos(a0),
                            c.centerY + circleRadius * std::sin(a0)},
                           {c.r, c.g, c.b, circleAlpha}};
        const Vertex rim1 {{c.centerX + circleRadius * std::cos(a1),
                            c.centerY + circleRadius * std::sin(a1)},
                           {c.r, c.g, c.b, circleAlpha}};

        out.push_back(centre);
        out.push_back(rim0);
        out.push_back(rim1);
    }
}

std::vector<Vertex> buildCircleMesh()
{
    auto out = std::vector<Vertex> {};
    out.reserve(sizeof(circles) / sizeof(circles[0]) * circleSegments * 3);
    for (const auto& c: circles)
        appendCircle(out, c);
    return out;
}

GeneratedShader makeBlendingShader()
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float4>();
    auto panelOffsetX = builder.uniform<Float>();
    auto varyingColor = builder.varying(color);

    builder.position(float4(position.x() + panelOffsetX, position.y(), 0.0f, 1.0f));
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
struct BlendingView final : GPUView
{
    BlendingView()
        : shader(makeBlendingShader())
        , vertexData(buildCircleMesh())
        , vertexBuffer(Device::shared().makeBuffer(
              vertexData.data(), vertexData.size() * sizeof(Vertex)))
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
        pass.draw((int) vertexData.size());
    }

    void render(Frame& frame) override
    {
        // Near-black clear so additive's overlaps have room to brighten toward
        // white without the single-circle regions saturating.
        auto pass = frame.beginPass({Graphics::Color {0.05f, 0.05f, 0.07f}});
        drawPanel(pass, none, -2.0f / 3.0f);
        drawPanel(pass, alphaBlend, 0.0f);
        drawPanel(pass, additive, +2.0f / 3.0f);
    }

    GeneratedShader shader;
    std::vector<Vertex> vertexData;
    Buffer vertexBuffer;
    ShaderLibrary library;
    RenderPipeline none;
    RenderPipeline alphaBlend;
    RenderPipeline additive;
};

// The 2D paint surface is transparent except where drawText lands, so the GPU
// pixels underneath show through.
struct LabelStripView final : Graphics::View
{
    void paint(Graphics::Context& g) override
    {
        struct Panel
        {
            const char* name;
            const char* expected;
        };
        const Panel panels[3] = {
            {"None", "blue overwrites all"},
            {"AlphaBlend", "murky blue-tinted mix"},
            {"Additive", "overlaps -> WHITE"},
        };

        // Approximate half-widths for monospaced Menlo, enough to centre a label.
        constexpr float nameHalfCharWidth = 4.5f;
        constexpr float expectedHalfCharWidth = 3.3f;
        constexpr float topBaselineY = 24.f;

        const auto bounds = getLocalBounds();
        const auto currentPanelWidth = bounds.w / 3.f;
        const auto bottomBaselineY = bounds.h - 18.f;

        g.setColor(Graphics::Color::white());

        for (auto i = 0; i < 3; ++i)
        {
            const auto panelCentreX = currentPanelWidth * ((float) i + 0.5f);

            const auto nameWidth =
                (float) std::strlen(panels[i].name) * nameHalfCharWidth * 2.f;
            const auto expectedWidth = (float) std::strlen(panels[i].expected)
                                       * expectedHalfCharWidth * 2.f;

            g.drawText(std::string {panels[i].name},
                       {panelCentreX - nameWidth * 0.5f, topBaselineY},
                       nameFont);
            g.drawText(std::string {panels[i].expected},
                       {panelCentreX - expectedWidth * 0.5f, bottomBaselineY},
                       expectedFont);
        }
    }

    Graphics::Font nameFont {
        Graphics::FontOptions().withName("Menlo").withSize(15.f)};
    Graphics::Font expectedFont {
        Graphics::FontOptions().withName("Menlo").withSize(11.f)};
};

// z-order is insertion order, so the labels added second sit on top.
struct RootView final : Graphics::View
{
    void resized() override
    {
        for (auto* child: getSubviews())
            child->setBounds(getLocalBounds());
    }
};

struct BlendingApp
{
    BlendingApp()
    {
        root.addSubview(blending);
        root.addSubview(labels);
        window.setContentView(root);
    }

    RootView root;
    BlendingView blending;
    LabelStripView labels;
    Graphics::Window window {windowOptions()};
};

int main()
{
    return eacp::Apps::run<BlendingApp>();
}
