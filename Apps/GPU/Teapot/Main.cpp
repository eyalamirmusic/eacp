#include <eacp/Graphics/Graphics.h>
#include <eacp/GPU/GPU.h>

#include "TeapotData.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
};

namespace
{
constexpr float pi = 3.14159265358979f;

float radians(float degrees)
{
    return degrees * (pi / 180.0f);
}

Vec3 operator-(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator+(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator*(Vec3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalize(Vec3 v)
{
    auto length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > 1.0e-6f ? v * (1.0f / length) : Vec3 {0.0f, 0.0f, 1.0f};
}

// Cubic Bernstein basis and its derivative, for evaluating a bicubic patch.
void bernstein(float t, float* basis, float* derivative)
{
    auto it = 1.0f - t;
    basis[0] = it * it * it;
    basis[1] = 3.0f * t * it * it;
    basis[2] = 3.0f * t * t * it;
    basis[3] = t * t * t;

    derivative[0] = -3.0f * it * it;
    derivative[1] = 3.0f * it * it - 6.0f * t * it;
    derivative[2] = 6.0f * t * it - 3.0f * t * t;
    derivative[3] = 3.0f * t * t;
}

Vec3 controlPoint(int patch, int i, int j)
{
    const auto& p = teapot::patches[patch][i * 4 + j];
    return {p[0], p[1], p[2]};
}

struct TeapotMesh
{
    Vector<Vertex> vertices;
    Vector<std::uint32_t> indices;
};

// Tessellates the 32 Bezier patches into a centred, unit-scaled indexed mesh
// with per-vertex normals (from the surface partial derivatives). Each patch
// shares its grid vertices between quads through the index buffer, so the
// upload is a fraction of the unindexed triangle list.
TeapotMesh buildTeapot()
{
    auto low = Vec3 {1e9f, 1e9f, 1e9f};
    auto high = Vec3 {-1e9f, -1e9f, -1e9f};

    for (auto p = 0; p < teapot::patchCount; ++p)
        for (auto k = 0; k < 16; ++k)
        {
            auto cp = controlPoint(p, k / 4, k % 4);
            low = {
                std::min(low.x, cp.x), std::min(low.y, cp.y), std::min(low.z, cp.z)};
            high = {std::max(high.x, cp.x),
                    std::max(high.y, cp.y),
                    std::max(high.z, cp.z)};
        }

    auto center = (low + high) * 0.5f;
    auto extent = std::max({high.x - low.x, high.y - low.y, high.z - low.z});
    auto scale = 2.0f / extent;

    constexpr auto steps = 12;
    constexpr auto rowLength = steps + 1;
    auto mesh = TeapotMesh {};

    for (auto p = 0; p < teapot::patchCount; ++p)
    {
        auto base = (std::uint32_t) mesh.vertices.size();

        for (auto iu = 0; iu <= steps; ++iu)
        {
            float bu[4], dbu[4];
            bernstein((float) iu / steps, bu, dbu);

            for (auto iv = 0; iv <= steps; ++iv)
            {
                float bv[4], dbv[4];
                bernstein((float) iv / steps, bv, dbv);

                auto position = Vec3 {0.0f, 0.0f, 0.0f};
                auto tangentU = Vec3 {0.0f, 0.0f, 0.0f};
                auto tangentV = Vec3 {0.0f, 0.0f, 0.0f};

                for (auto i = 0; i < 4; ++i)
                    for (auto j = 0; j < 4; ++j)
                    {
                        auto cp = controlPoint(p, i, j);
                        position = position + cp * (bu[i] * bv[j]);
                        tangentU = tangentU + cp * (dbu[i] * bv[j]);
                        tangentV = tangentV + cp * (bu[i] * dbv[j]);
                    }

                auto normal = normalize(cross(tangentU, tangentV));
                auto centred = (position - center) * scale;
                mesh.vertices.add({centred, normal});
            }
        }

        for (auto iu = 0; iu < steps; ++iu)
            for (auto iv = 0; iv < steps; ++iv)
            {
                auto a = base + (std::uint32_t) (iu * rowLength + iv);
                auto b = a + rowLength;
                auto c = b + 1;
                auto d = a + 1;

                mesh.indices.getVector().insert(mesh.indices.end(),
                                                {a, b, c, a, c, d});
            }
    }

    return mesh;
}

// Per-vertex (Gouraud) shaded teapot. The whole transform pipeline - model spin,
// view, perspective - is built in the shader from two scalar uniforms (angle and
// aspect); the CPU uploads no matrices at all. Lighting is computed in the vertex
// stage and handed to the fragment as a varying.
struct TeapotShader final : ShaderProgram
{
    TeapotShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto normal = vertexInput(&Vertex::normal);

        auto model = rotateZ(angle);
        auto view = translate(0.0f, -0.15f, -3.2f) * rotateX(radians(-72.0f));
        auto projection = perspective(aspect, radians(45.0f), 0.1f, 100.0f);
        auto modelView = view * model;

        setPosition(projection * modelView * float4(position, 1.0f));

        auto worldNormal = normalize((modelView * float4(normal, 0.0f)).xyz());
        auto toLight = normalize(lightDir);

        // Two-sided diffuse term: |N . L|, so inward-facing patches still light.
        auto diffuse = abs(dot(worldNormal, toLight));
        auto shade = diffuse * 0.8f + 0.2f;

        setFragment(float4(varying(baseColor * shade), 1.0f));
    }

    Uniform<Float> angle;
    Uniform<Float> aspect;
    Uniform<Float3> lightDir;
    Uniform<Float3> baseColor;

    EACP_SHADER(angle, aspect, lightDir, baseColor)
};
} // namespace

struct TeapotView final : GPUView
{
    TeapotView()
        : mesh(buildTeapot())
    {
        setDepth(true);
        shader.setVertices(mesh.vertices.data(), mesh.vertices.size());
        shader.setIndices(mesh.indices.data(), mesh.indices.size());
        shader.prepare(sampleCount(), true);
        setContinuous(true);
    }

    void update(Threads::FrameTime time) override
    {
        spin += radiansPerSecond * static_cast<float>(time.delta);
    }

    void render(Frame& frame) override
    {
        auto bounds = getLocalBounds();

        // The CPU uploads only scalars now; the shader builds every matrix.
        shader.angle = spin;
        shader.aspect = bounds.h > 0.0f ? bounds.w / bounds.h : 1.0f;
        shader.lightDir = std::array {0.4f, 0.5f, 0.8f};
        shader.baseColor = std::array {0.85f, 0.5f, 0.32f};

        auto pass = frame.beginPass({Graphics::Color {0.09f, 0.10f, 0.13f}});
        pass.draw(shader);
    }

    static constexpr float radiansPerSecond = 0.6f;

    TeapotMesh mesh;
    TeapotShader shader;
    float spin = 0.0f;
};

struct MyApp
{
    MyApp() { window.setContentView(teapot); }

    TeapotView teapot;
    Graphics::Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
