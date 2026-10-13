#pragma once

#include <eacp/GPU/GPU.h>

// Every program the tiles draw with, written once in the shader EDSL so the
// same source becomes MSL, HLSL and GLSL. Nothing here knows it is drawing into
// a component rather than a view: the pass a tile opens is the host's frame
// redirected at the tile's texture, and a pipeline only has to agree with that
// texture's sample count, depth and format.

namespace GPUComponentsDemo
{
using namespace eacp;

struct QuadCorner
{
    float position[2];
};

constexpr QuadCorner fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

struct MeshVertex
{
    Maths::Vec3 position;
    Maths::Vec3 normal;
};

struct Particle
{
    float position[2];
    float velocity[2];
};

struct LineInstance
{
    float from[2];
    float to[2];
    float colour[4];
};

struct Splat
{
    float centre[2];
    float size[2];
    float colour[4];
};

// A lit mesh: the matrices come from the CPU, the lighting is per fragment so
// the highlight slides over the torus as it turns.
struct MeshShader final : GPU::ShaderProgram
{
    MeshShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&MeshVertex::position);
        auto normal = vertexInput(&MeshVertex::normal);

        setPosition(viewProjection * model * float4(position, 1.f));

        auto turned = varying((model * float4(normal, 0.f)).xyz());
        auto n = normalize(turned);
        auto l = normalize(lightDirection);
        auto halfway = normalize(l + float3(constant(0.f), constant(0.f), 1.f));

        auto diffuse = max(dot(n, l), 0.f);
        auto specular = pow(max(dot(n, halfway), 0.f), 40.f);

        setFragment(
            float4(baseColour * (diffuse * 0.8f + 0.18f) + specular * 0.7f, 1.f));
    }

    GPU::Uniform<GPU::Float4x4> model;
    GPU::Uniform<GPU::Float4x4> viewProjection;
    GPU::Uniform<GPU::Float3> lightDirection;
    GPU::Uniform<GPU::Float3> baseColour;

    EACP_SHADER(model, viewProjection, lightDirection, baseColour)
};

// One damped-spring step per particle, from one buffer into the other.
struct IntegrateParticles final : GPU::ComputeProgram
{
    IntegrateParticles() { compile(); }

    void define() override
    {
        auto index = threadId();
        auto particle = state.read4(index);

        auto px = particle.x();
        auto py = particle.y();
        auto vx = particle.z();
        auto vy = particle.w();

        auto stiffness = pull * (0.3f + fract(toFloat(index) * 0.6180339887f) * 2.f);

        auto nvx = (vx + (attractorX - px) * stiffness * timeStep) * damping;
        auto nvy = (vy + (attractorY - py) * stiffness * timeStep) * damping;

        write(
            next, index, float4(px + nvx * timeStep, py + nvy * timeStep, nvx, nvy));
    }

    GPU::Uniform<GPU::InputBuffer> state;
    GPU::Uniform<GPU::OutputBuffer> next;
    GPU::Uniform<GPU::Float> attractorX;
    GPU::Uniform<GPU::Float> attractorY;
    GPU::Uniform<GPU::Float> pull;
    GPU::Uniform<GPU::Float> damping;
    GPU::Uniform<GPU::Float> timeStep;

    EACP_SHADER(state, next, attractorX, attractorY, pull, damping, timeStep)
};

// A soft dot per particle, read straight out of the buffer the kernel wrote.
struct DrawParticles final : GPU::ShaderProgram
{
    DrawParticles() { compile(); }

    void define() override
    {
        auto corner = vertexInput(&QuadCorner::position);
        auto centre = instanceInput(&Particle::position, 1);
        auto velocity = instanceInput(&Particle::velocity, 1);

        setPosition(float4(centre.x() + corner.x() * radius / aspect,
                           centre.y() + corner.y() * radius,
                           0.f,
                           1.f));

        auto across = varying(corner);
        auto speed = varying(length(velocity));

        auto glow = 1.f - smoothstep(0.f, 1.f, length(across));
        auto slow = float3(constant(0.15f), constant(0.45f), constant(1.f));
        auto fast = float3(constant(1.f), constant(0.75f), constant(0.35f));
        auto tint = mix(slow, fast, clamp(speed * 0.35f, 0.f, 1.f));

        setFragment(float4(tint * glow * brightness, glow * brightness));
    }

    GPU::Uniform<GPU::Float> radius;
    GPU::Uniform<GPU::Float> aspect;
    GPU::Uniform<GPU::Float> brightness;

    EACP_SHADER(radius, aspect, brightness)
};

// A thin line as a quad a fixed number of pixels wide, which is where MSAA
// earns its keep: at one sample a half-pixel line is a staircase of gaps.
struct DrawLines final : GPU::ShaderProgram
{
    DrawLines() { compile(); }

    void define() override
    {
        auto corner = vertexInput(&QuadCorner::position);
        auto from = instanceInput(&LineInstance::from, 1);
        auto to = instanceInput(&LineInstance::to, 1);
        auto colour = instanceInput(&LineInstance::colour, 1);

        auto a = (from * 0.5f + 0.5f) * pixelSize;
        auto b = (to * 0.5f + 0.5f) * pixelSize;
        auto direction = normalize(b - a + float2(constant(0.0001f), 0.f));
        auto across = float2(-direction.y(), direction.x());

        auto along = mix(a, b, corner.x() * 0.5f + 0.5f);
        auto placed = along + across * corner.y() * halfWidth;

        setPosition(float4(placed / pixelSize * 2.f - 1.f, 0.f, 1.f));
        setFragment(varying(colour));
    }

    GPU::Uniform<GPU::Float2> pixelSize;
    GPU::Uniform<GPU::Float> halfWidth;

    EACP_SHADER(pixelSize, halfWidth)
};

// A full-quad plasma whose alpha falls off towards the edges, written straight
// and blended onto a transparent clear so the texture holds premultiplied
// colour.
struct PlasmaShader final : GPU::ShaderProgram
{
    PlasmaShader() { compile(); }

    void define() override
    {
        auto corner = vertexInput(&QuadCorner::position);
        setPosition(float4(corner, 0.f, 1.f));

        auto p = varying(corner) * float2(aspect, constant(1.f)) * 3.f;

        auto wave = sin(p.x() + time) + sin(p.y() * 1.3f + time * 0.7f)
                    + sin((p.x() + p.y()) * 0.7f + time * 1.3f)
                    + sin(length(p) * 1.5f - time * 1.1f);

        auto phase = wave * 0.785f;
        auto colour = float3(sin(phase) * 0.5f + 0.5f,
                             sin(phase + 2.094f) * 0.5f + 0.5f,
                             sin(phase + 4.188f) * 0.5f + 0.5f);

        auto edge = length(varying(corner));
        auto alpha = (1.f - smoothstep(0.35f, 1.f, edge)) * strength;

        setFragment(float4(colour, alpha));
    }

    GPU::Uniform<GPU::Float> time;
    GPU::Uniform<GPU::Float> aspect;
    GPU::Uniform<GPU::Float> strength;

    EACP_SHADER(time, aspect, strength)
};

// The previous feedback frame, sampled slightly zoomed and turned and faded,
// which is what turns a handful of splats into trails.
struct FeedbackShader final : GPU::ShaderProgram
{
    FeedbackShader()
    {
        previous.sampling = {GPU::TextureFilter::Linear,
                             GPU::TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&QuadCorner::position);
        setPosition(float4(corner, 0.f, 1.f));

        auto centred = varying(corner) * zoom;
        auto c = cos(twist);
        auto s = sin(twist);
        auto turned = float2(centred.x() * c - centred.y() * s,
                             centred.x() * s + centred.y() * c);

        auto uv = float2(turned.x() * 0.5f + 0.5f, 0.5f - turned.y() * 0.5f);
        auto colour = sample(previous, uv);

        setFragment(float4(colour.xyz() * decay, 1.f));
    }

    GPU::Uniform<GPU::Texture2D> previous;
    GPU::Uniform<GPU::Float> zoom;
    GPU::Uniform<GPU::Float> twist;
    GPU::Uniform<GPU::Float> decay;

    EACP_SHADER(previous, zoom, twist, decay)
};

// Soft round discs, one per instance, for the feedback emitters and the
// on-demand tile's clicks.
struct DrawSplats final : GPU::ShaderProgram
{
    DrawSplats() { compile(); }

    void define() override
    {
        auto corner = vertexInput(&QuadCorner::position);
        auto centre = instanceInput(&Splat::centre, 1);
        auto size = instanceInput(&Splat::size, 1);
        auto colour = instanceInput(&Splat::colour, 1);

        setPosition(float4(centre.x() + corner.x() * size.x() / aspect,
                           centre.y() + corner.y() * size.x(),
                           0.f,
                           1.f));

        auto across = length(varying(corner));
        auto ring = varying(size.y());

        auto disc = 1.f - smoothstep(0.75f, 1.f, across);
        auto hole = smoothstep(ring - 0.15f, ring, across);
        auto coverage = disc * hole;

        setFragment(float4(varying(colour).xyz(), varying(colour).w() * coverage));
    }

    GPU::Uniform<GPU::Float> aspect;

    EACP_SHADER(aspect)
};

// A texture drawn into a rectangle of the target, in clip space: the feedback
// tile's picture across the whole of it and its thumbnail in a corner.
struct BlitShader final : GPU::ShaderProgram
{
    BlitShader()
    {
        image.sampling = {GPU::TextureFilter::Linear,
                          GPU::TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&QuadCorner::position);
        auto placed = rect.xy() + (corner * 0.5f + 0.5f) * rect.zw();

        setPosition(float4(placed, 0.f, 1.f));

        auto across = varying(corner);
        auto uv = float2(across.x() * 0.5f + 0.5f, 0.5f - across.y() * 0.5f);

        setFragment(float4(sample(image, uv).xyz() * tint, 1.f));
    }

    GPU::Uniform<GPU::Texture2D> image;
    GPU::Uniform<GPU::Float4> rect;
    GPU::Uniform<GPU::Float> tint;

    EACP_SHADER(image, rect, tint)
};

// A flat fill over the whole target with a band of colour stepping through a
// count, so a render that did not happen is a colour that did not change.
struct CounterShader final : GPU::ShaderProgram
{
    CounterShader() { compile(); }

    void define() override
    {
        auto corner = vertexInput(&QuadCorner::position);
        setPosition(float4(corner, 0.f, 1.f));

        auto p = varying(corner);
        auto bands = fract(p.x() * 2.f + p.y() * 0.5f + count * 0.137f);
        auto stripe = smoothstep(0.45f, 0.5f, bands) - smoothstep(0.95f, 1.f, bands);

        setFragment(float4(mix(background, highlight, stripe * 0.35f), 1.f));
    }

    GPU::Uniform<GPU::Float> count;
    GPU::Uniform<GPU::Float3> background;
    GPU::Uniform<GPU::Float3> highlight;

    EACP_SHADER(count, background, highlight)
};
} // namespace GPUComponentsDemo
