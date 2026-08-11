#include <eacp/Graphics/Graphics.h>
#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

struct Vec2
{
    float x, y;
};

struct Color
{
    float r, g, b;
};

struct Vertex
{
    Vec2 position;
    Color color;
};

EACP_SHADER_VALUE(Vec2, Float2)
EACP_SHADER_VALUE(Color, Float3)

namespace
{
const Vertex triangleVertices[] = {
    {{0.0f, 0.8f}, {1.0f, 0.2f, 0.2f}},
    {{-0.8f, -0.8f}, {0.2f, 1.0f, 0.2f}},
    {{0.8f, -0.8f}, {0.2f, 0.2f, 1.0f}},
};

struct RotatingShader final : ShaderProgram
{
    RotatingShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto color = vertexInput(&Vertex::color);
        auto varyingColor = varying(color);

        auto c = cos(angle);
        auto s = sin(angle);
        auto px = position.x();
        auto py = position.y();
        auto rotated = float2(px * c - py * s, px * s + py * c);

        setPosition(float4(rotated, 0.0f, 1.0f));
        setFragment(float4(varyingColor, 1.0f));
    }

    Uniform<Float> angle;

    EACP_SHADER(angle)
};
} // namespace

struct RotatingTriangleView final : GPUView
{
    RotatingTriangleView()
    {
        shader.setVertices(triangleVertices);
        shader.prepare(sampleCount());
        setContinuous(true);
    }

    void update(Threads::FrameTime time) override
    {
        angle += radiansPerSecond * static_cast<float>(time.delta);
    }

    void render(Frame& frame) override
    {
        shader.angle = angle;

        auto pass = frame.beginPass({});
        pass.draw(shader);
    }

    static constexpr float radiansPerSecond = 1.2f;

    RotatingShader shader;
    float angle = 0.0f;
};

struct MyApp
{
    MyApp() { window.setContentView(triangle); }

    RotatingTriangleView triangle;
    Graphics::Window window;
};

int main()
{
    return eacp::Apps::run<MyApp>();
}
