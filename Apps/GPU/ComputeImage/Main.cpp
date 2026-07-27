#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

// Compute to fragment: a kernel paints an image, the very next render pass
// samples it, and the pixels never touch the CPU.
//
// The kernel is dispatched over a grid rather than a flat count -
// threadPosition() gives it the x and y of the texel it owns - and its output
// is a Texture created with computeWrite, not a buffer. That is the whole
// difference from ComputeParticles, whose kernel writes a float buffer the
// vertex stage reads as a per-instance stream.
//
// The texture is a fixed 512 x 512 and the quad stretches it over whatever the
// window is, so resizing costs nothing: no resource is recreated, and the
// kernel dispatches the same grid every frame.

namespace
{
constexpr int imageSize = 512;

struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
// Two triangles covering clip space, so the fragment stage runs once per pixel
// of the window.
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{+1.f, -1.f}},
    {{-1.f, +1.f}},
    {{+1.f, -1.f}},
    {{+1.f, +1.f}},
    {{-1.f, +1.f}},
};

// A plasma: four travelling waves summed and mapped through a two-colour ramp.
// One work item per texel, so the whole image is one dispatch and the CPU
// contributes a single scalar - what time it is.
struct PaintPlasma final : ComputeProgram
{
    PaintPlasma() { compile(); }

    void define() override
    {
        auto p = threadPosition();

        auto x = toFloat(p.x) * texelSize;
        auto y = toFloat(p.y) * texelSize;

        auto radius = length(float2(x - 0.5f, y - 0.5f));

        auto wave = sin(x * 12.f + time) + sin(y * 9.f - time * 0.7f)
                    + sin((x + y) * 7.f + time * 1.3f)
                    + sin(radius * 24.f - time * 2.f);

        // Four waves land in [-4, 4]; the ramp wants [0, 1].
        auto shade = wave * 0.125f + 0.5f;

        auto cool = float3(constant(0.04f), constant(0.20f), constant(0.65f));
        auto warm = float3(constant(1.00f), constant(0.55f), constant(0.15f));

        write(target, p.x, p.y, float4(mix(cool, warm, shade), 1.f));
    }

    Uniform<WritableTexture2D> target;
    Uniform<Float> time;
    Uniform<Float> texelSize;

    EACP_SHADER(target, time, texelSize)
};

// The consumer: an ordinary textured full-screen quad. Nothing here knows the
// texture came from a kernel - it is bound and sampled exactly as an uploaded
// one is, which is the point.
struct DrawImage final : ShaderProgram
{
    DrawImage()
    {
        // Linear, so the 512-texel image stays smooth when the window is
        // larger than it is.
        image.sampling = {TextureFilter::Linear, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = varying(position * 0.5f + 0.5f);
        auto centred = varying(position);

        setPosition(float4(position, 0.f, 1.f));

        // A vignette, so it is visible that a fragment stage is doing its own
        // work with what the kernel handed it rather than blitting it through.
        auto falloff = 1.f - smoothstep(0.6f, 1.5f, length(centred));

        setFragment(float4(sample(image, uv).xyz() * falloff, 1.f));
    }

    Uniform<Texture2D> image;

    EACP_SHADER(image)
};

TextureDescriptor describeTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = imageSize;
    descriptor.height = imageSize;

    // Only the formats a typed UAV store is guaranteed for may ask for this;
    // BGRA8Unorm - the drawable's own format - is not one of them. See
    // supportsComputeWrite.
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.computeWrite = true;
    return descriptor;
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 800;
    options.height = 800;
    options.title = "eacp - Compute Image";
    options.minWidth = 320;
    options.minHeight = 320;
    return options;
}
} // namespace

struct ComputeImageView final : GPUView
{
    ComputeImageView()
        : target(Device::shared().makeTexture(describeTarget()))
    {
        kernel.target = target;
        kernel.texelSize = 1.f / (float) imageSize;
        kernel.prepare();

        renderer.setVertices(fullQuad);
        renderer.image = target;
        renderer.prepare(sampleCount());

        setContinuous(true);
    }

    void update(Threads::FrameTime frameTime) override
    {
        elapsed += (float) frameTime.delta;
    }

    void render(Frame& frame) override
    {
        paintImage(frame);

        auto pass = frame.beginPass({Graphics::Color {0.f, 0.f, 0.f}});
        pass.draw(renderer);
    }

    // The compute pass. Its encoder ends when this returns, which is what
    // orders the kernel's writes before the sample the render pass takes of
    // them - the same rule two render passes on a frame follow.
    void paintImage(Frame& frame)
    {
        auto pass = frame.beginCompute();

        kernel.time = elapsed;
        pass.dispatch(kernel, imageSize, imageSize);
    }

    Texture target;
    PaintPlasma kernel;
    DrawImage renderer;

    float elapsed = 0.f;
};

struct ComputeImageApp
{
    ComputeImageApp() { window.setContentView(view); }

    ComputeImageView view;
    Graphics::Window window {windowOptions()};
};

int main()
{
    return eacp::Apps::run<ComputeImageApp>();
}
