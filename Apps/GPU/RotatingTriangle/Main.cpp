#include <eacp/Core/Threads/Timer.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

namespace
{
struct Vertex
{
    float position[2];
    float color[3];
};

const Vertex triangleVertices[] = {
    {{0.0f, 0.8f}, {1.0f, 0.2f, 0.2f}},
    {{-0.8f, -0.8f}, {0.2f, 1.0f, 0.2f}},
    {{0.8f, -0.8f}, {0.2f, 0.2f, 1.0f}},
};

// The shader is an object: its vertex inputs and the per-frame angle are named,
// typed members. EACP_SHADER lists them so the framework can walk the members to
// build the IR + vertex layout and to pack the uniform block - the angle the CPU
// sets each frame flows in through the named member, not a positional slot.
struct RotatingShader final : ShaderProgram
{
    RotatingShader() { compile(); }

    void define() override
    {
        auto varyingColor = varying(color);

        auto c = cos(angle);
        auto s = sin(angle);
        auto px = position.x();
        auto py = position.y();
        auto rotated = float2(px * c - py * s, px * s + py * c);

        setPosition(float4(rotated, 0.0f, 1.0f));
        setFragment(float4(varyingColor, 1.0f));
    }

    VertexInput<Float2> position {&Vertex::position};
    VertexInput<Float3> color {&Vertex::color};
    Uniform<Float> angle;

    EACP_SHADER(position, color, angle)
};
} // namespace

struct RotatingTriangleView final : GPUView
{
    RotatingTriangleView()
        : vertexBuffer(Device::shared().makeBuffer(triangleVertices))
        , library(Device::shared().makeShaderLibrary(shader.source()))
        , pipeline(makePipeline())
        , timer([this] { advance(); }, 60)
    {
    }

    RenderPipeline makePipeline()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout = shader.vertexLayout();

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void advance()
    {
        angle += 0.02f;
        repaint();
    }

    void render(Frame& frame) override
    {
        shader.angle = angle;

        auto pass = frame.beginPass();
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.setVertexUniforms(shader);
        pass.draw(3);
    }

    RotatingShader shader;
    Buffer vertexBuffer;
    ShaderLibrary library;
    RenderPipeline pipeline;
    float angle = 0.0f;
    Threads::Timer timer;
};

struct MyApp
{
    MyApp() { window.setContentView(triangle); }

    RotatingTriangleView triangle;
    Graphics::Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
