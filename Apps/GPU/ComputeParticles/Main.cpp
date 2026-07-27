#include <eacp/GPU/GPU.h>

#include <eacp/Core/Utils/Containers.h>

#include <algorithm>
#include <cmath>

using namespace eacp;
using namespace GPU;

// Frame::beginCompute: a kernel and the draw that consumes it, on one command
// buffer, in one frame.
//
// 40,000 particles are integrated by a compute pass and drawn by the very next
// render pass, out of the buffer the kernel just wrote. The state is uploaded
// once at startup and is never read back, never re-uploaded, and never seen by
// the CPU again - each frame the CPU contributes four scalars (where the
// attractor is, how long the step was) and nothing else.
//
// Without beginCompute the integration would have to run on a separate
// CommandBuffer whose commit() blocks until the GPU is finished, every frame,
// before the draw could touch a byte of it. Here the two passes are ordered by
// the queue, exactly as two render passes on a frame are, and nothing waits.

namespace
{
constexpr int particleCount = 40000;

// The one layout the two stages agree on. The kernel writes it as a flat run of
// floats at floatsPerParticle * index; the vertex shader reads the same memory
// as a per-instance stream with this struct's stride. Nothing converts between
// the two views - there is only ever one copy.
struct Particle
{
    float position[2];
    float velocity[2];
};

constexpr int floatsPerParticle = (int) (sizeof(Particle) / sizeof(float));
static_assert(floatsPerParticle == 4, "the kernel indexes state in fours");

constexpr auto stateBytes = sizeof(Particle) * (std::size_t) particleCount;

// The quad every particle is drawn as: two triangles of corners in [-1, 1],
// scaled to the particle radius by the vertex shader. One instance per
// particle, so this is the only per-vertex data in the program.
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

    // The golden angle spreads successive indices evenly over the disc rather
    // than into the arms a constant angle step would leave.
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

// The state buffer, uploaded with that starting disc. A function rather than a
// member holding the vector: the CPU copy is scaffolding for one upload and has
// no reason to outlive the constructor.
Buffer makeStateBuffer()
{
    auto particles = makeInitialParticles();
    return {Device::shared(), particles.data(), stateBytes, BufferUsage::Storage};
}

// One Euler step per particle: a damped spring towards the attractor. Reads the
// whole state from one buffer and writes it to another, because a kernel may
// not read the buffer it is writing - which is what the two buffers the view
// swaps between are for.
struct IntegrateParticles final : ComputeProgram
{
    IntegrateParticles() { compile(); }

    void define() override
    {
        auto index = threadId();
        auto base = index * 4u;

        auto px = state[base];
        auto py = state[base + 1u];
        auto vx = state[base + 2u];
        auto vy = state[base + 3u];

        // A per-particle stiffness, spread by the golden ratio so neighbouring
        // indices land far apart rather than in bands. Without it every
        // particle feels the same force, follows the same path, and 40,000 of
        // them draw one dot. The spread is wide because it is the only thing
        // keeping the swarm a cloud rather than a trail.
        auto stiffness =
            pull * (0.25f + fract(toFloat(index) * 0.6180339887f) * 2.f);

        // Bounded by construction: a damped oscillator driven by a bounded
        // target cannot run away, which an inverse-square attraction very much
        // can as soon as a particle passes close to the centre.
        auto nvx = (vx + (attractorX - px) * stiffness * timeStep) * damping;
        auto nvy = (vy + (attractorY - py) * stiffness * timeStep) * damping;

        write(next, base, px + nvx * timeStep);
        write(next, base + 1u, py + nvy * timeStep);
        write(next, base + 2u, nvx);
        write(next, base + 3u, nvy);
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

// Draws the state buffer without owning it: the per-vertex stream is the six
// quad corners, and slot 1 is pointed straight at whichever buffer the kernel
// wrote this frame (see setInstanceBuffer in render()).
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

        // A round, soft dot: bright at the centre, gone by the quad's edge, so
        // the square the quad actually is never shows.
        auto glow = 1.f - smoothstep(0.f, 1.f, length(acrossQuad));

        auto slow = float3(constant(0.10f), constant(0.35f), constant(1.00f));
        auto fast = float3(constant(1.00f), constant(0.80f), constant(0.40f));
        auto tint = mix(slow, fast, clamp(speed * speedScale, 0.f, 1.f));

        // Dim, because additive blending accumulates: at full brightness the
        // first few overlapping particles clip to white and the density the
        // blend mode exists to show is lost.
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
        // pointed at a buffer instead of given CPU data. Additive blending is
        // what makes overlapping particles read as density.
        renderer.setInstanceBuffer(1, bufferB, particleCount);
        renderer.prepare(
            sampleCount(), false, PrimitiveTopology::Triangles, BlendMode::Additive);

        setContinuous(true);
    }

    void update(Threads::FrameTime time) override
    {
        elapsed += (float) time.delta;

        // Clamped, because a stalled frame would otherwise hand the integrator
        // a step long enough to throw the whole swarm off screen in one go.
        step = std::min((float) time.delta, 1.f / 30.f);
    }

    void render(Frame& frame) override
    {
        // The two buffers alternate roles every frame: what the kernel wrote
        // last frame is what it reads this frame. Both are only ever touched by
        // GPU commands on one queue, which executes them in submission order,
        // so the alternation needs no fence and no CPU synchronisation - the
        // frames-in-flight pipelining is on the CPU side and cannot reorder it.
        auto& source = front ? bufferA : bufferB;
        auto& target = front ? bufferB : bufferA;

        integrate(frame, source, target);
        drawParticles(frame, target);

        front = !front;
    }

    // The compute pass. Its encoder ends when this returns, which is what
    // orders the kernel's writes before anything the render pass reads.
    void integrate(Frame& frame, const Buffer& source, const Buffer& target)
    {
        auto pass = frame.beginCompute();

        integrator.state = source;
        integrator.next = target;

        // The attractor traces a Lissajous figure, so the swarm is chasing a
        // target that never quite repeats the same sweep.
        integrator.attractorX = 0.55f * std::cos(elapsed * 0.7f);
        integrator.attractorY = 0.45f * std::sin(elapsed * 1.1f);
        integrator.pull = 6.0f;

        // Barely damped: enough to keep the integrator from gaining energy, not
        // enough to pull the swarm into the attractor and leave a trail.
        integrator.damping = 0.999f;
        integrator.timeStep = step;

        pass.dispatch(integrator, particleCount);
    }

    // The render pass, reading the buffer the compute pass above just wrote.
    // This is the whole point: no readback, no second command buffer, no wait.
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
