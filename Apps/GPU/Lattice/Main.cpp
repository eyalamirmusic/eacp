#include <eacp/Graphics/Graphics.h>
#include <eacp/GPU/GPU.h>

#include <algorithm>
#include <array>
#include <cmath>

using namespace eacp;
using namespace GPU;

// A 3-float value used for both CPU mesh math and as a Float3 vertex attribute.
struct Vec3
{
    float x, y, z;
    using ShaderValue = Float3;
};

struct Vertex
{
    Vec3 position;
    Vec3 normal;
    Vec3 color;
};

namespace
{
constexpr float pi = 3.14159265358979f;

float radians(float degrees)
{
    return degrees * (pi / 180.0f);
}

Vec3 operator+(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator*(Vec3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

struct Mesh
{
    Vector<Vertex> vertices;
    Vector<std::uint32_t> indices;
};

// Adds one axis-aligned cube (centre, half-extent, flat colour) as six
// quads with outward face normals, sharing corners per face through the
// index buffer.
void addCube(Mesh& mesh, Vec3 center, float half, Vec3 color)
{
    struct Face
    {
        Vec3 normal, u, v; // u x v == normal, so corners wind CCW from outside
    };

    constexpr Face faces[] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };

    for (const auto& face: faces)
    {
        auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        auto faceCenter = center + face.normal * half;

        auto corner = [&](float du, float dv)
        { return faceCenter + face.u * (du * half) + face.v * (dv * half); };

        mesh.vertices.add({corner(-1, -1), face.normal, color});
        mesh.vertices.add({corner(1, -1), face.normal, color});
        mesh.vertices.add({corner(1, 1), face.normal, color});
        mesh.vertices.add({corner(-1, 1), face.normal, color});

        for (auto i: {0u, 1u, 2u, 0u, 2u, 3u})
            mesh.indices.add(base + i);
    }
}

// A cubic lattice of cubes, coloured by position so the cluster reads as a
// smooth RGB gradient in space.
Mesh buildLattice()
{
    constexpr auto n = 4;
    constexpr auto step = 0.9f;
    constexpr auto half = 0.28f;

    auto mesh = Mesh {};
    auto coord = [](int i) { return (i - (n - 1) * 0.5f) * step; };

    for (auto i = 0; i < n; ++i)
        for (auto j = 0; j < n; ++j)
            for (auto k = 0; k < n; ++k)
            {
                auto unit = [](int x) { return 0.2f + 0.8f * (x / float(n - 1)); };
                addCube(mesh,
                        {coord(i), coord(j), coord(k)},
                        half,
                        {unit(i), unit(j), unit(k)});
            }

    return mesh;
}

// Orbit-camera lattice. yaw/pitch/distance drive the view; the whole
// transform is built in the shader from those scalars (no CPU matrices).
struct LatticeShader final : ShaderProgram
{
    Uniform<Float> yaw;
    Uniform<Float> pitch;
    Uniform<Float> distance;
    Uniform<Float> aspect;
    Uniform<Float3> lightDir;

    EACP_SHADER(yaw, pitch, distance, aspect, lightDir)

    LatticeShader() { compile(); }

    // rotateX with a dynamic (uniform) angle — the built-in rotateX only
    // takes a compile-time float, so build the matrix from sin/cos here.
    Float4x4 pitchMatrix(const Float& angle)
    {
        auto c = cos(angle);
        auto s = sin(angle);
        auto z0 = constant(0.0f);
        auto o = constant(1.0f);
        return float4x4(float4(o, z0, z0, z0),
                        float4(z0, c, s, z0),
                        float4(z0, -s, c, z0),
                        float4(z0, z0, z0, o));
    }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto normal = vertexInput(&Vertex::normal);
        auto color = vertexInput(&Vertex::color);

        auto camera = translate(constant(0.0f), constant(0.0f), distance * -1.0f)
                      * pitchMatrix(pitch) * rotateY(yaw);
        auto projection = perspective(aspect, radians(45.0f), 0.1f, 100.0f);

        setPosition(projection * camera * float4(position, 1.0f));

        auto worldNormal = normalize((camera * float4(normal, 0.0f)).xyz());
        auto toLight = normalize(lightDir);
        auto diffuse = abs(dot(worldNormal, toLight));
        auto shade = diffuse * 0.75f + 0.25f;

        setFragment(float4(varying(color * shade), 1.0f));
    }
};

// A full-screen overlay quad. The glass shape is carved out procedurally in
// the fragment stage, so the geometry is just two triangles in clip space.
struct GlassVertex
{
    float position[2];
    float uv[2];
};

constexpr GlassVertex glassQuad[] = {
    {{-1.0f, -1.0f}, {0.0f, 1.0f}},
    {{1.0f, -1.0f}, {1.0f, 1.0f}},
    {{1.0f, 1.0f}, {1.0f, 0.0f}},
    {{-1.0f, -1.0f}, {0.0f, 1.0f}},
    {{1.0f, 1.0f}, {1.0f, 0.0f}},
    {{-1.0f, 1.0f}, {0.0f, 0.0f}},
};

// A procedural "liquid glass" panel, blended over the lattice. It's a rounded
// rectangle (signed-distance field) carrying a bright refractive rim, animated
// caustic highlights and a drifting specular blob — all driven by a time
// uniform so it stays alive. Translucent, so the lattice shows through.
struct GlassShader final : ShaderProgram
{
    Uniform<Float> time;
    Uniform<Float> aspect;

    EACP_SHADER(time, aspect)

    GlassShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&GlassVertex::position);
        auto uv = vertexInput(&GlassVertex::uv);

        setPosition(float4(position, 0.0f, 1.0f));

        auto coord = varying(uv);
        auto px = (coord.x() - constant(0.5f)) * aspect;
        auto py = coord.y() - constant(0.5f);

        // Signed distance to a rounded rectangle (negative inside).
        auto rad = constant(0.10f);
        auto qx = abs(px) - constant(0.30f) + rad;
        auto qy = abs(py) - constant(0.30f) + rad;
        auto outside =
            length(float2(max(qx, constant(0.0f)), max(qy, constant(0.0f))));
        auto inside = min(max(qx, qy), constant(0.0f));
        auto sdf = outside + inside - rad;

        // Soft-edged panel coverage, and a bright fresnel-ish rim at the border.
        auto panel =
            constant(1.0f) - smoothstep(constant(-0.004f), constant(0.004f), sdf);
        auto rim = smoothstep(constant(0.05f), constant(0.0f), abs(sdf));

        // Animated liquid caustics — layered moving sine fields.
        auto w1 = sin(px * constant(11.0f) + time * constant(1.1f))
                  * sin(py * constant(11.0f) - time * constant(0.8f));
        auto w2 = sin((px + py) * constant(16.0f) - time * constant(1.6f));
        auto caustic = clamp(
            w1 * constant(0.5f) + constant(0.5f), constant(0.0f), constant(1.0f));
        auto spec = pow(caustic, constant(4.0f));

        // A specular blob drifting across the surface.
        auto bx = sin(time * constant(0.6f)) * constant(0.12f);
        auto by = cos(time * constant(0.5f)) * constant(0.12f);
        auto blob = smoothstep(
            constant(0.16f), constant(0.0f), length(float2(px - bx, py - by)));

        auto tint = float3(constant(0.50f), constant(0.68f), constant(0.95f));
        auto white = float3(constant(1.0f), constant(1.0f), constant(1.0f));
        auto highlight = spec * constant(0.5f) + rim * constant(0.9f)
                         + blob * constant(0.5f) + w2 * constant(0.04f);
        auto color = tint * constant(0.30f) + white * highlight;

        auto alpha = panel
                     * clamp(constant(0.16f) + highlight * constant(0.8f),
                             constant(0.0f),
                             constant(0.95f));

        setFragment(float4(color, alpha));
    }
};
} // namespace

struct LatticeView final : GPUView
{
    LatticeView()
        : mesh(buildLattice())
    {
        // Opt into mouse + keyboard input so a real mouse (and the agent,
        // through the same dispatch path) can orbit/zoom it.
        getProperties().handlesMouseEvents = true;
        getProperties().grabsFocusOnMouseDown = true;

        setDepth(true);
        shader.setVertices(mesh.vertices.data(), mesh.vertices.size());
        shader.setIndices(mesh.indices.data(), mesh.indices.size());
        shader.prepare(sampleCount(), true);

        // The glass overlay: no depth (it draws on top), blending on (it's
        // translucent over the lattice).
        glass.setVertices(glassQuad);
        glass.prepare(sampleCount(), false, true);

        setContinuous(true);
    }

    void update(Threads::FrameTime time) override
    {
        elapsed += static_cast<float>(time.delta);
        if (autoSpin && !dragging)
            yaw += idleSpinPerSecond * static_cast<float>(time.delta);
    }

    void render(Frame& frame) override
    {
        auto bounds = getLocalBounds();
        auto aspect = bounds.h > 0.0f ? bounds.w / bounds.h : 1.0f;

        shader.yaw = yaw;
        shader.pitch = pitch;
        shader.distance = distance;
        shader.aspect = aspect;
        shader.lightDir = std::array {0.4f, 0.6f, 0.7f};

        auto pass = frame.beginPass({Graphics::Color {0.05f, 0.06f, 0.09f}});
        pass.draw(shader);

        // Liquid-glass panel composited over the 3D scene in the same pass.
        glass.time = elapsed;
        glass.aspect = aspect;
        pass.draw(glass);
    }

    // Interactivity — the same handlers a real mouse/keyboard reach, and
    // the ones the debug server's input tools drive.
    void mouseDown(const Graphics::MouseEvent&) override { dragging = true; }
    void mouseUp(const Graphics::MouseEvent&) override { dragging = false; }

    void mouseDragged(const Graphics::MouseEvent& event) override
    {
        yaw += event.delta.x * dragSpeed;
        pitch = std::clamp(pitch + event.delta.y * dragSpeed, -1.4f, 1.4f);
        repaint();
    }

    void mouseWheel(const Graphics::MouseEvent& event) override
    {
        distance = std::clamp(distance - event.delta.y * zoomSpeed, 2.5f, 16.0f);
        repaint();
    }

    void keyDown(const Graphics::KeyEvent& event) override
    {
        if (event.characters == " ")
            autoSpin = !autoSpin;
        else if (event.characters == "r" || event.characters == "R")
        {
            yaw = 0.6f;
            pitch = 0.5f;
            distance = 7.0f;
        }
    }

    static constexpr float idleSpinPerSecond = 0.35f;
    static constexpr float dragSpeed = 0.01f;
    static constexpr float zoomSpeed = 0.05f;

    Mesh mesh;
    LatticeShader shader;
    GlassShader glass;
    float yaw = 0.6f;
    float pitch = 0.5f;
    float distance = 7.0f;
    float elapsed = 0.0f;
    bool autoSpin = true;
    bool dragging = false;
};

struct MyApp
{
    MyApp() { window.setContentView(lattice); }

    LatticeView lattice;
    Graphics::Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
