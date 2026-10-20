#include "ViewABI.h"

#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

namespace
{
constexpr auto tau = 6.2831853f;

struct QuadVertex
{
    Maths::Vec2 position;
};

constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{+1.f, -1.f}},
    {{-1.f, +1.f}},
    {{+1.f, -1.f}},
    {{+1.f, +1.f}},
    {{-1.f, +1.f}},
};

using RGB = float[3];

struct FullScreenProgram : ShaderProgram
{
    Float2 screenPosition()
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto centred = varying(position);
        return float2(centred.x() * aspect, centred.y());
    }

    Float3 rgb(const RGB& colour)
    {
        return float3(constant(colour[0]), constant(colour[1]), constant(colour[2]));
    }

    Float3 palette(const Float& value,
                   const RGB& base,
                   const RGB& amplitude,
                   const RGB& frequency,
                   const RGB& phase)
    {
        auto angle = (rgb(frequency) * value + rgb(phase)) * tau;
        return rgb(base) + rgb(amplitude) * cos(angle);
    }

    Uniform<Float> time;
    Uniform<Float> aspect;
};

struct PlasmaShader final : FullScreenProgram
{
    static constexpr auto speed = 0.6f;
    static constexpr auto zoom = 2.2f;
    static constexpr auto warp = 0.9f;
    static constexpr auto bands = 1.4f;
    static constexpr RGB base = {0.5f, 0.5f, 0.5f};
    static constexpr RGB amplitude = {0.5f, 0.5f, 0.5f};
    static constexpr RGB frequency = {1.0f, 1.0f, 0.5f};
    static constexpr RGB phase = {0.8f, 0.9f, 0.3f};

    PlasmaShader() { compile(); }

    void define() override
    {
        auto t = time * speed;
        auto p = screenPosition() * zoom;

        auto first = float2(sin(p.x() * 1.7f + t + sin(p.y() * 2.3f - t * 0.7f)),
                            cos(p.y() * 1.3f - t * 0.8f + cos(p.x() * 2.9f + t)));
        auto warped = p + first * warp;

        auto second = float2(sin(warped.y() * 2.5f + t * t * 1.1f),
                             cos(warped.x() * 1.9f - t * 1.3f));
        auto twiceWarped = warped + second * warp;

        auto field = sin(twiceWarped.x() * 3.f) * cos(twiceWarped.y() * 3.f)
                     + sin(length(twiceWarped) * 4.f - t * 2.f);
        auto value = field * 0.25f * bands + length(second) * 0.35f + t * 0.1f;

        auto colour = palette(value, base, amplitude, frequency, phase);
        auto sheen = smoothstep(0.75f, 1.f, sin(value * tau * 2.f)) * 0.35f;

        setFragment(float4(colour + sheen, 1.f));
    }

    EACP_SHADER(time, aspect)
};

struct TunnelShader final : FullScreenProgram
{
    static constexpr auto speed = 4.5f;
    static constexpr auto spin = 0.25f;
    static constexpr auto twist = 0.6f;
    static constexpr auto stripes = 12.f;
    static constexpr auto rings = 3.f;
    static constexpr auto depth = 0.5f;
    static constexpr auto hueShift = 0.15f;
    static constexpr auto coreGlow = 0.08f;
    static constexpr RGB coreColour = {1.0f, 0.55f, 0.9f};
    static constexpr RGB base = {0.5f, 0.5f, 0.5f};
    static constexpr RGB amplitude = {0.5f, 0.5f, 0.5f};
    static constexpr RGB frequency = {1.0f, 1.0f, 1.0f};
    static constexpr RGB phase = {0.0f, 0.33f, 0.67f};

    TunnelShader() { compile(); }

    void define() override
    {
        auto p = screenPosition();
        auto wobble = float2(sin(time * 0.7f), cos(time * 0.9f)) * 0.15f;
        auto offset = p - wobble;

        auto radius = max(length(offset), 0.001f);
        auto angle = atan2(offset.y(), offset.x());
        auto distance = depth / radius + time * speed;

        auto around = angle * (stripes / tau) + distance * twist + time * spin;
        auto stripe = smoothstep(0.3f, 0.48f, abs(fract(around) - 0.5f));
        auto ring = smoothstep(0.85f, 1.f, abs(sin(distance * rings)));

        auto hue = palette(distance * hueShift, base, amplitude, frequency, phase);
        auto fog = smoothstep(0.f, 0.7f, radius);
        auto walls = hue * (stripe * 0.9f + ring * 1.4f + 0.08f) * fog;
        auto glow = rgb(coreColour) * (coreGlow / radius);

        setFragment(float4(walls + glow, 2.f));
    }

    EACP_SHADER(time, aspect)
};

template <typename Shader>
struct FullScreenView final : GPUView
{
    FullScreenView()
    {
        shader.setVertices(fullQuad);
        shader.prepare(sampleCount());
        setContinuous(true);
    }

    void update(Threads::FrameTime frameTime) override
    {
        elapsed += (float) frameTime.delta;
    }

    void render(Frame& frame) override
    {
        auto size = frame.logicalSize();
        shader.time = elapsed;
        shader.aspect = size.y > 0.f ? size.x / size.y : 1.f;

        auto pass = frame.beginPass({});
        pass.draw(shader);
    }

    Shader shader;
    float elapsed = 0.f;
};

using PlasmaView = FullScreenView<PlasmaShader>;
using TunnelView = FullScreenView<TunnelShader>;
} // namespace

EACP_HOT_RELOAD_EXPORT ViewPointer makeDemoView()
{
    // return makeOwned<TunnelView>();
    return makeOwned<PlasmaView>();
}
