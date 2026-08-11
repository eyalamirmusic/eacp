#include <eacp/GPU/GPU.h>

#include <eacp/Core/Utils/Containers.h>

#include <algorithm>
#include <cmath>

using namespace eacp;
using namespace GPU;

namespace
{
constexpr int particleCount = 40000;

// The kernel reads and writes this as a four-float record; the vertex shader
// reads the same memory as a per-instance stream with this struct's stride.
struct Particle
{
    float position[2];
    float velocity[2];
};

constexpr int floatsPerParticle = (int) (sizeof(Particle) / sizeof(float));
static_assert(floatsPerParticle == 4, "the kernel reads state with read4");

constexpr auto stateBytes = sizeof(Particle) * (std::size_t) particleCount;

// Corners in [-1, 1], scaled to the particle radius by the vertex shader.
struct Corner
{
    float offset[2];
};

constexpr Corner quadCorners[] = {
    {{-1.f, -1.f}},
    {{+1.f, -1.f}},
    {{+1.f, +1.f}},
    {{-1.f, -1.f}},
    {{+1.f, +1.f}},
    {{-1.f, +1.f}},
};

// A disc of particles, each already moving tangentially, so the swarm has
// angular momentum before the attractor does anything to it.
Vector<Particle> makeInitialParticles()
{
    auto particles = Vector<Particle> {};
    particles.reserve(particleCount);

    // Spreads successive indices over the disc rather than into arms.
    constexpr auto goldenAngle = 2.39996323f;

    for (auto i = 0; i < particleCount; ++i)
    {
        auto radius = 0.9f * std::sqrt((float) i / (float) particleCount);
        auto angle = goldenAngle * (float) i;

        auto x = radius * std::cos(angle);
        auto y = radius * std::sin(angle);

        particles.create() = {{x, y}, {-y * 0.9f, x * 0.9f}};
    }

    return particles;
}

Buffer makeStateBuffer()
{
    auto particles = makeInitialParticles();
    return {Device::shared(), particles.data(), stateBytes, BufferUsage::Storage};
}

// One Euler step per particle: a damped spring towards the attractor. A kernel
// may not read the buffer it is writing, hence the two buffers the view swaps.
struct IntegrateParticles final : ComputeProgram
{
    IntegrateParticles() { compile(); }

    void define() override
    {
        auto index = threadId();

        // read4 and write index in particles, not in floats.
        auto particle = state.read4(index);

        auto px = particle.x();
        auto py = particle.y();
        auto vx = particle.z();
        auto vy = particle.w();

        // Spread by the golden ratio: at one shared stiffness all 40,000
        // particles follow the same path and draw a single dot.
        auto stiffness =
            pull * (0.25f + fract(toFloat(index) * 0.6180339887f) * 2.f);

        auto nvx = (vx + (attractorX - px) * stiffness * timeStep) * damping;
        auto nvy = (vy + (attractorY - py) * stiffness * timeStep) * damping;

        write(
            next, index, float4(px + nvx * timeStep, py + nvy * timeStep, nvx, nvy));
    }

    Uniform<InputBuffer> state;
    Uniform<OutputBuffer> next;
    Uniform<Float> attractorX;
    Uniform<Float> attractorY;
    Uniform<Float> pull;
    Uniform<Float> damping;
    Uniform<Float> timeStep;

    EACP_SHADER(state, next, attractorX, attractorY, pull, damping, timeStep)
};

struct DrawParticles final : ShaderProgram
{
    DrawParticles() { compile(); }

    void define() override
    {
        auto corner = vertexInput(&Corner::offset);
        auto centre = instanceInput(&Particle::position, 1);
        auto velocity = instanceInput(&Particle::velocity, 1);

        // The quad is square in pixels, so its x extent shrinks by the aspect.
        setPosition(float4(centre.x() + corner.x() * radius / aspect,
                           centre.y() + corner.y() * radius,
                           0.f,
                           1.f));

        auto acrossQuad = varying(corner);
        auto speed = varying(length(velocity));

        auto glow = 1.f - smoothstep(0.f, 1.f, length(acrossQuad));

        auto slow = float3(constant(0.10f), constant(0.35f), constant(1.00f));
        auto fast = float3(constant(1.00f), constant(0.80f), constant(0.40f));
        auto tint = mix(slow, fast, clamp(speed * speedScale, 0.f, 1.f));

        // Dim, because additive blending accumulates: at full brightness the
        // first few overlapping particles already clip to white.
        setFragment(float4(tint * glow * brightness, glow * brightness));
    }

    Uniform<Float> radius;
    Uniform<Float> aspect;
    Uniform<Float> speedScale;
    Uniform<Float> brightness;

    EACP_SHADER(radius, aspect, speedScale, brightness)
};

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 900;
    options.height = 700;
    options.title = "eacp - Compute Particles";
    options.minWidth = 320;
    options.minHeight = 240;
    return options;
}
} // namespace

struct ParticleView final : GPUView
{
    ParticleView()
        : bufferA(makeStateBuffer())
        , bufferB(Device::shared(), nullptr, stateBytes, BufferUsage::Storage)
    {
        integrator.prepare();

        renderer.setVertices(quadCorners);

        // Slot 1 is fed by the kernel rather than by setInstances, so it is
        // pointed at a buffer instead of given CPU data.
        renderer.setInstanceBuffer(1, bufferB, particleCount);
        renderer.prepare(
            sampleCount(), false, PrimitiveTopology::Triangles, BlendMode::Additive);

        setContinuous(true);
    }

    void update(Threads::FrameTime time) override
    {
        elapsed += (float) time.delta;

        // Clamped: a stalled frame would otherwise hand the integrator a step
        // long enough to throw the whole swarm off screen in one go.
        step = std::min((float) time.delta, 1.f / 30.f);
    }

    void render(Frame& frame) override
    {
        // The two buffers alternate roles every frame. Both are only ever
        // touched by GPU commands on one queue, which runs them in submission
        // order, so the alternation needs no fence and no CPU synchronisation.
        auto& source = front ? bufferA : bufferB;
        auto& target = front ? bufferB : bufferA;

        integrate(frame, source, target);
        drawParticles(frame, target);

        front = !front;
    }

    // The encoder ends when this returns, which is what orders the kernel's
    // writes before anything the render pass reads.
    void integrate(Frame& frame, const Buffer& source, const Buffer& target)
    {
        auto pass = frame.beginCompute();

        integrator.state = source;
        integrator.next = target;

        integrator.attractorX = 0.55f * std::cos(elapsed * 0.7f);
        integrator.attractorY = 0.45f * std::sin(elapsed * 1.1f);
        integrator.pull = 6.0f;

        integrator.damping = 0.999f;
        integrator.timeStep = step;

        pass.dispatch(integrator, particleCount);
    }

    void drawParticles(Frame& frame, const Buffer& particles)
    {
        auto pass = frame.beginPass({Graphics::Color {0.02f, 0.02f, 0.05f}});
        auto bounds = getLocalBounds();

        renderer.setInstanceBuffer(1, particles, particleCount);
        renderer.radius = 0.005f;
        renderer.aspect = bounds.h > 0.f ? bounds.w / bounds.h : 1.f;
        renderer.speedScale = 0.30f;
        renderer.brightness = 0.55f;

        pass.drawInstanced(renderer, particleCount);
    }

    Buffer bufferA;
    Buffer bufferB;

    IntegrateParticles integrator;
    DrawParticles renderer;

    float elapsed = 0.f;
    float step = 1.f / 60.f;
    bool front = true;
};

struct ComputeParticlesApp
{
    ComputeParticlesApp() { window.setContentView(view); }

    ParticleView view;
    Graphics::Window window {windowOptions()};
};

int main()
{
    return eacp::Apps::run<ComputeParticlesApp>();
}
