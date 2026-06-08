#include <eacp/Core/Threads/Timer.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

// Reusable value sub-types instead of raw float arrays. EACP_SHADER_VALUE teaches
// the shader layer their shape once, so they stand in for float2 / float3 wherever
// a vertex field or uniform is expected.
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

// The shader is an object. The angle is a named uniform you set; the vertex inputs
// are pulled straight out of the Vertex struct in define(), so that struct is the
// single source of the vertex layout - no separate input declarations to keep in
// sync, and the layout reads the fields' real offsets.
struct RotatingShader final : ShaderProgram
{
    Uniform<Float> angle;

    EACP_SHADER(angle)

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
};
} // namespace

struct RotatingTriangleView final : GPUView
{
    RotatingTriangleView()
    {
        shader.setVertices(triangleVertices);
        shader.prepare(sampleCount());
        eacp::LOG (shader.source().source);
    }

    void advance()
    {
        angle += 0.02f;
        repaint();
    }

    void render(Frame& frame) override
    {
        shader.angle = angle;

        auto pass = frame.beginPass({});
        pass.draw(shader);
    }

    RotatingShader shader;
    float angle = 0.0f;
    Threads::Timer timer {[this] { advance(); }, 60};
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
