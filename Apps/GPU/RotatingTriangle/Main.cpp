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

// Same vertex-coloured triangle, but the vertex position is rotated by an angle
// the CPU feeds in each frame as a uniform. The rotation maths is generated into
// the shader (sin/cos); only the angle changes per frame.
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
} // namespace

struct RotatingTriangleView final : GPUView
{
    RotatingTriangleView()
        : shader(makeRotatingShader())
        , vertexBuffer(Device::shared().makeBuffer(triangleVertices))
        , library(Device::shared().makeShaderLibrary(shader.source))
        , pipeline(makePipeline())
        , timer([this] { advance(); }, 60)
    {
    }

    RenderPipeline makePipeline()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout = shader.vertexLayout;

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void advance()
    {
        angle += 0.02f;
        repaint();
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.10f, 0.10f, 0.12f}});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.setVertexUniform(angle);
        pass.draw(3);
    }

    GeneratedShader shader;
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
