#include "RotatingShader.h"

#include <eacp/GPU/Shader/PrecompiledShaders.h>
#include <eacp/Graphics/Graphics.h>

using namespace RotatingTriangle;

namespace
{
const Vertex triangleVertices[] = {
    {{0.0f, 0.8f}, {1.0f, 0.2f, 0.2f}},
    {{-0.8f, -0.8f}, {0.2f, 1.0f, 0.2f}},
    {{0.8f, -0.8f}, {0.2f, 0.2f, 1.0f}},
};

// Whether the build compiled the shader ahead of time (Shaders.cpp and
// eacp_add_precompiled_shaders) or the device compiled its source just now.
void logShaderOrigin(const ShaderProgram& shader)
{
    LOG("RotatingTriangle: shader ",
        shader.isPrecompiled() ? "precompiled" : "compiled at runtime",
        " (",
        Precompiled::embeddedCount(),
        " precompiled blobs embedded)");
}
} // namespace

// Continuous mode renders every display refresh, synchronized with vsync.
// update() advances the animation by the frame's delta time, so the rotation
// speed is the same on any refresh rate and unaffected by skipped frames.
struct RotatingTriangleView final : GPUView
{
    RotatingTriangleView()
    {
        shader.setVertices(triangleVertices);
        shader.prepare(sampleCount());
        logShaderOrigin(shader);
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

int main()
{
    return Graphics::runWindowedApp<RotatingTriangleView>();
}
