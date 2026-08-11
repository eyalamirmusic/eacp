#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

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
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{+1.f, -1.f}},
    {{-1.f, +1.f}},
    {{+1.f, -1.f}},
    {{+1.f, +1.f}},
    {{-1.f, +1.f}},
};

// One work item per texel: four travelling waves through a two-colour ramp.
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

// The kernel's texture is bound and sampled exactly as an uploaded one is.
struct DrawImage final : ShaderProgram
{
    DrawImage()
    {
        // Linear, so the fixed 512-texel image stays smooth when stretched.
        image.sampling = {TextureFilter::Linear, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        auto uv = varying(position * 0.5f + 0.5f);
        auto centred = varying(position);

        setPosition(float4(position, 0.f, 1.f));

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

    // computeWrite needs a format guaranteed for typed UAV stores; BGRA8Unorm -
    // the drawable's own format - is not one of them. See supportsComputeWrite.
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

    // The encoder ends when this returns, which is what orders the kernel's
    // writes before the render pass samples them.
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
