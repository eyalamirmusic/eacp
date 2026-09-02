#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// A pass reading the depth an earlier pass wrote, and the effect that is the
// whole reason to want it: soft particles.
//
// A translucent quad drawn into a scene is a flat sheet, and where it passes
// through the floor it says so - the floor cuts it along a perfectly straight
// line, which is the one thing a puff of steam never does. The fix is not more
// geometry. It is for the fragment to *ask how far away the floor is*: compare
// its own distance from the camera against the distance the depth buffer
// recorded there, and fade out as the two converge. The sheet then dissolves
// into whatever it meets instead of being sliced by it. It is what Doom 3's
// soft particles do, and porting them is what this eacp feature was built for.
//
// What the fragment has to read is the depth buffer of a pass that has already
// finished, which is three things put together:
//
//   TextureDescriptor::sampleableDepth   a render target whose depth plane may
//                                        be sampled and not only attached
//   DepthAction::Keep                    that pass keeping what it wrote, so
//                                        there is still something to sample
//   ShaderBuilder::depthTexture          the shader slot, bound with
//                                        RenderPass::setFragmentDepthTexture -
//                                        which takes the *render target*, a
//                                        depth attachment having no separate
//                                        existence - and whose sample() gives
//                                        back one float rather than four
//
// The order is the rule and not a suggestion: a texture cannot be sampled by
// the pass rendering into it, and the depth plane is a texture. So the scene
// goes into an off-screen target first, that pass ends, and everything after it
// reads what it left.
//
// Four panels, one scene, one camera, so the mechanism is visible rather than
// described:
//
//   colour           the off-screen target's colour plane, drawn straight out
//   depth            its *depth* plane, sampled through depthTexture() and
//                    linearised against the same near/far the scene is drawn
//                    with (0.5 and 20 units) - near is black, the far plane is
//                    white
//   hard particles   six camera-facing puffs over the scene, depth-tested and
//                    nothing more: each one is cut off along a straight line
//                    where it enters the floor
//   soft particles   the same six, fading over 0.4 units as their own distance
//                    approaches the distance the depth buffer holds. The panel
//                    this app exists for.
//
// Every panel is exactly the off-screen target's width, and every panel uses the
// same projection it was rendered with, so a fragment's own clip position names
// the texel of that target it is standing on. That is what makes the fade a
// lookup rather than a second render.
//
// TextureFormat::R32Float rides along, and not as a footnote: it is the format a
// depth copy has to land in. A fifth pass copies the depth out into one with a
// full-screen draw, and --check reads it back and prints what the same value
// would have survived as in eight bits and in a half float. The fade distance is
// 0.4 world units and eight bits cannot express it at all - see the numbers.
//
// Run with --check to render one frame headlessly and print those numbers plus
// the two particle panels as luminance grids, which is how the soft panel is
// confirmed to differ from the hard one without anyone having to look at a
// window. It exits non-zero if it does not.

using namespace eacp;
using namespace GPU;

namespace
{
constexpr auto panelCount = 4;
constexpr auto pi = 3.14159265358979f;

// The projection every panel and the off-screen target share. They have to
// share it: the fade reads the depth buffer at the fragment's own clip
// position, and two projections would put the fragment and the texel it looks
// up in different places.
constexpr auto nearPlane = 0.5f;
constexpr auto farPlane = 20.f;
constexpr auto fieldOfView = 46.f;

// How far in front of the geometry a particle has to be to reach full opacity.
// Chosen against the scene rather than for the look: a puff is about a metre
// across, so a fade this wide is visible without swallowing the whole quad -
// and it is small enough that an eight-bit depth copy could not express it,
// which is the argument --check prints in numbers.
constexpr auto fadeDistance = 0.4f;

constexpr auto backgroundColor = Graphics::Color {0.05f, 0.06f, 0.08f};

float radians(float degrees)
{
    return degrees * (pi / 180.f);
}

// Window-space depth back to the distance from the camera it stands for. The
// buffer holds almost none of its range where the geometry is - half of it is
// spent inside the first metre - so anything measured in world units has to come
// back through this first.
float linearDistance(float depth)
{
    return (nearPlane * farPlane) / (farPlane - depth * (farPlane - nearPlane));
}

struct Vec2
{
    float x = 0.f;
    float y = 0.f;

    using ShaderValue = Float2;
};

struct Vec3
{
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    using ShaderValue = Float3;
};

Vec3 operator+(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(Vec3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalize(Vec3 v)
{
    auto length = std::sqrt(dot(v, v));
    return length > 1.0e-6f ? v * (1.f / length) : Vec3 {0.f, 0.f, 1.f};
}

// Column-major, matching the shader EDSL's own transform builders and the
// Float4x4 both languages agree on the layout of.
using Mat4 = Array<float, 16>;

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    auto out = Mat4 {};

    for (auto column = 0; column < 4; ++column)
        for (auto row = 0; row < 4; ++row)
        {
            auto sum = 0.f;

            for (auto k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[column * 4 + k];

            out[column * 4 + row] = sum;
        }

    return out;
}

// The same right-handed [0, 1]-depth projection ShaderProgram::perspective
// builds in the shader, and the one linearDistance above inverts.
Mat4 perspective(float aspect, float fovY, float nearZ, float farZ)
{
    auto f = 1.f / std::tan(fovY * 0.5f);
    auto out = Mat4 {};
    out.fill(0.f);

    out[0] = f / aspect;
    out[5] = f;
    out[10] = farZ / (nearZ - farZ);
    out[11] = -1.f;
    out[14] = (farZ * nearZ) / (nearZ - farZ);

    return out;
}

Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up)
{
    auto forward = normalize(eye - target);
    auto right = normalize(cross(up, forward));
    auto trueUp = cross(forward, right);

    auto out = Mat4 {};

    out[0] = right.x;
    out[1] = trueUp.x;
    out[2] = forward.x;
    out[3] = 0.f;
    out[4] = right.y;
    out[5] = trueUp.y;
    out[6] = forward.y;
    out[7] = 0.f;
    out[8] = right.z;
    out[9] = trueUp.z;
    out[10] = forward.z;
    out[11] = 0.f;
    out[12] = -dot(right, eye);
    out[13] = -dot(trueUp, eye);
    out[14] = -dot(forward, eye);
    out[15] = 1.f;

    return out;
}

struct Camera
{
    Mat4 viewProjection {};

    // The billboard basis, so a puff faces whatever the camera became this
    // frame. Kept beside the matrix because they come out of the same three
    // vectors and would otherwise be recovered from it.
    Vec3 right {};
    Vec3 up {};
};

struct SceneVertex
{
    Vec3 position;
    Vec3 normal;
    Vec3 color;
};

// A billboard corner: where it is in the world, and where it is on the quad.
// The second is what the round falloff is computed from - a square puff would
// have a hard edge of its own and confuse the one being demonstrated.
struct PuffVertex
{
    Vec3 position;
    Vec2 corner;
};

// A full-screen triangle pair in clip space. No texture coordinate: the
// position *is* the coordinate, one flip away, and saying so once is better
// than carrying the flip in the data.
struct QuadVertex
{
    Vec2 position;
};

constexpr QuadVertex fullScreenQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

// Clip space to the texture coordinate that reads the same point of a render
// target: x straight across, y flipped, because clip space has y up and row 0
// of a texture is the top. Both the full-screen passes and the particle fade go
// through this, which is the point of it being one function - a fade looking up
// a mirrored texel would still fade, just around the wrong geometry.
Float2 targetCoordinate(const Float2& ndc)
{
    return float2(ndc.x() * 0.5f + 0.5f, 0.5f - ndc.y() * 0.5f);
}

// linearDistance above, in the shader. The two have to agree: the fade compares
// a number this produced against one the CPU never sees, and --check reports
// distances the CPU computed from depths the GPU wrote.
Float linearDistance(const Float& depth)
{
    return (nearPlane * farPlane) / (farPlane - depth * (farPlane - nearPlane));
}

// A round puff out of a square quad, falling off from the middle to nothing at
// the rim.
Float puffCoverage(const Float2& corner)
{
    return smoothstep(0.f, 0.85f, 1.f - length(corner));
}

// The floor and the boxes, lit by one direction so the shapes read. Nothing
// here is about depth sampling; it is what the depth buffer is a picture of.
struct SceneShader final : ShaderProgram
{
    SceneShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&SceneVertex::position);
        auto normal = vertexInput(&SceneVertex::normal);
        auto color = vertexInput(&SceneVertex::color);

        setPosition(viewProjection * float4(position, 1.f));

        auto worldNormal = normalize(varying(normal));
        auto lambert = max(dot(worldNormal, lightDirection), 0.f);

        setFragment(float4(varying(color) * (0.24f + lambert * 0.86f), 1.f));
    }

    Uniform<Float4x4> viewProjection;
    Uniform<Float3> lightDirection;

    EACP_SHADER(viewProjection, lightDirection)
};

// Panel 0: the colour plane of the off-screen target, one texel to one pixel.
struct ColorPanelShader final : ShaderProgram
{
    ColorPanelShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(sample(sceneColor, varying(targetCoordinate(position))));
    }

    Uniform<Texture2D> sceneColor;

    EACP_SHADER(sceneColor)
};

// Panel 1: the *depth* plane of that same target, through the slot this whole
// example is about. sample() on one of these is a Float and not a Float4 -
// there is one channel there and never was another.
struct DepthPanelShader final : ShaderProgram
{
    DepthPanelShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));

        auto depth = sample(sceneDepth, varying(targetCoordinate(position)));

        // Straight out, the buffer is a white sheet with a hint of grey at the
        // very front: it spends half its range inside the first metre. What is
        // worth looking at is the distance behind it.
        auto distance = linearDistance(depth);
        auto ramp = clamp((distance - nearPlane) / (farPlane - nearPlane), 0.f, 1.f);

        setFragment(float4(ramp, ramp, ramp, 1.f));
    }

    Uniform<TextureDepth2D> sceneDepth;

    EACP_SHADER(sceneDepth)
};

// The depth of the off-screen target, straight out into a single-channel float
// target - the copy an R32Float texture exists for, and what --check reads back.
struct DepthCopyShader final : ShaderProgram
{
    DepthCopyShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));

        auto depth = sample(sceneDepth, varying(targetCoordinate(position)));

        setFragment(float4(depth, depth, depth, 1.f));
    }

    Uniform<TextureDepth2D> sceneDepth;

    EACP_SHADER(sceneDepth)
};

// Panel 2: a puff that knows nothing about what is behind it. The depth test
// keeps it out of the floor and that is all the floor does to it - so the quad
// ends along a line, which is the artefact.
struct PuffShader final : ShaderProgram
{
    PuffShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&PuffVertex::position);
        auto corner = vertexInput(&PuffVertex::corner);

        setPosition(viewProjection * float4(position, 1.f));
        setFragment(float4(tint, puffCoverage(varying(corner)) * opacity));
    }

    Uniform<Float4x4> viewProjection;
    Uniform<Float3> tint;
    Uniform<Float> opacity;

    EACP_SHADER(viewProjection, tint, opacity)
};

// Panel 3: the same puff, asking how far away the geometry behind it is.
//
// Everything new is in the fragment stage and it is four lines: where this
// fragment is on the target, how far away it is, how far away the depth buffer
// says the scene is there, and the difference over the fade distance. The clip
// position has to come through as a varying to get either - there is no
// fragment-coordinate builtin, and this is the value that would feed one.
struct SoftPuffShader final : ShaderProgram
{
    SoftPuffShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&PuffVertex::position);
        auto corner = vertexInput(&PuffVertex::corner);

        auto clipPosition = viewProjection * float4(position, 1.f);
        setPosition(clipPosition);

        auto clip = varying(clipPosition);
        auto ndc = clip.xy() / clip.w();

        auto ownDistance = linearDistance(clip.z() / clip.w());
        auto sceneDistance =
            linearDistance(sample(sceneDepth, targetCoordinate(ndc)));

        // Zero where the puff has reached the surface, one a fade distance in
        // front of it. Clamped rather than tested: the depth test has already
        // thrown away everything behind the geometry, so the negative half of
        // this is the sliver the rasterizer let through.
        auto fade = clamp((sceneDistance - ownDistance) / fadeDistance, 0.f, 1.f);

        setFragment(float4(tint, puffCoverage(varying(corner)) * opacity * fade));
    }

    Uniform<Float4x4> viewProjection;
    Uniform<Float3> tint;
    Uniform<Float> opacity;

    // The *render target* is assigned to this, not a texture of its own - a
    // depth attachment is created with its colour texture and lives exactly as
    // long. The uniform binder turns that into
    // RenderPass::setFragmentDepthTexture.
    Uniform<TextureDepth2D> sceneDepth;

    EACP_SHADER(viewProjection, tint, opacity, sceneDepth)
};

constexpr auto floorHeight = 0.f;
constexpr auto floorExtent = 6.f;

struct Box
{
    Vec3 centre;
    Vec3 halfSize;
    Vec3 color;
};

// Three of them at three distances, which is what gives the depth buffer
// something to say: a puff in front of the near box and a puff behind the far
// one fade against quite different numbers.
constexpr Box boxes[] = {
    {{-1.75f, 0.55f, 0.4f}, {0.75f, 0.55f, 0.75f}, {0.78f, 0.44f, 0.28f}},
    {{1.55f, 0.4f, 1.15f}, {0.55f, 0.4f, 0.55f}, {0.32f, 0.55f, 0.68f}},
    {{0.35f, 1.15f, -2.3f}, {0.36f, 1.15f, 0.36f}, {0.62f, 0.62f, 0.68f}},
};

constexpr auto boxCount = (int) (sizeof(boxes) / sizeof(boxes[0]));

// Five faces each, not six: every box stands on the floor and nothing ever sees
// its underside.
constexpr auto sceneVertexCount = 6 + boxCount * 5 * 6;

struct Puff
{
    Vec3 centre;
    float radius = 1.f;
};

// Every one of them has its centre closer to the floor than its radius, so the
// lower half of the quad is under the floor and the intersection is on screen.
// Which is the entire subject: a sheet of steam ending in a straight line.
constexpr Puff puffs[] = {
    {{-1.7f, 0.42f, 1.65f}, 1.f},
    {{0.45f, 0.34f, 0.55f}, 0.85f},
    {{1.75f, 0.46f, 2.1f}, 0.95f},
    {{-0.55f, 0.5f, -1.35f}, 1.05f},
    {{1.1f, 0.3f, -0.95f}, 0.8f},
    {{-2.5f, 0.38f, -0.5f}, 0.9f},
};

constexpr auto puffCount = (int) (sizeof(puffs) / sizeof(puffs[0]));
constexpr auto puffVertexCount = puffCount * 6;

void appendQuad(std::vector<SceneVertex>& out,
                Vec3 a,
                Vec3 b,
                Vec3 c,
                Vec3 d,
                Vec3 normal,
                Vec3 color)
{
    out.push_back({a, normal, color});
    out.push_back({b, normal, color});
    out.push_back({c, normal, color});
    out.push_back({a, normal, color});
    out.push_back({c, normal, color});
    out.push_back({d, normal, color});
}

void appendBox(std::vector<SceneVertex>& out, const Box& box)
{
    auto c = box.centre;
    auto h = box.halfSize;

    auto corner = [&](int x, int y, int z)
    {
        return Vec3 {
            c.x + (float) x * h.x, c.y + (float) y * h.y, c.z + (float) z * h.z};
    };

    appendQuad(out,
               corner(-1, -1, 1),
               corner(1, -1, 1),
               corner(1, 1, 1),
               corner(-1, 1, 1),
               {0.f, 0.f, 1.f},
               box.color);
    appendQuad(out,
               corner(1, -1, -1),
               corner(-1, -1, -1),
               corner(-1, 1, -1),
               corner(1, 1, -1),
               {0.f, 0.f, -1.f},
               box.color);
    appendQuad(out,
               corner(1, -1, 1),
               corner(1, -1, -1),
               corner(1, 1, -1),
               corner(1, 1, 1),
               {1.f, 0.f, 0.f},
               box.color);
    appendQuad(out,
               corner(-1, -1, -1),
               corner(-1, -1, 1),
               corner(-1, 1, 1),
               corner(-1, 1, -1),
               {-1.f, 0.f, 0.f},
               box.color);
    appendQuad(out,
               corner(-1, 1, 1),
               corner(1, 1, 1),
               corner(1, 1, -1),
               corner(-1, 1, -1),
               {0.f, 1.f, 0.f},
               box.color);
}

std::vector<SceneVertex> buildScene()
{
    auto vertices = std::vector<SceneVertex> {};
    vertices.reserve(sceneVertexCount);

    appendQuad(vertices,
               {-floorExtent, floorHeight, floorExtent},
               {floorExtent, floorHeight, floorExtent},
               {floorExtent, floorHeight, -floorExtent},
               {-floorExtent, floorHeight, -floorExtent},
               {0.f, 1.f, 0.f},
               {0.30f, 0.33f, 0.38f});

    for (const auto& box: boxes)
        appendBox(vertices, box);

    return vertices;
}

TextureDescriptor describeSceneTarget(int width, int height)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;

    // The whole request, and it implies depth: there is nothing to sample
    // without a buffer to sample. What it buys over plain `depth` is that a
    // later pass may read it - which on D3D12 is a typeless resource, a
    // shader-visible descriptor and a barrier per pass, and is why the two are
    // separate questions.
    descriptor.sampleableDepth = true;

    return descriptor;
}

TextureDescriptor describeDepthCopy(int width, int height)
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = TextureFormat::R32Float;
    descriptor.renderTarget = true;
    return descriptor;
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1280;
    options.height = 420;
    options.minWidth = 720;
    options.minHeight = 260;
    options.title = "eacp GPU - Depth Sampling";
    return options;
}
} // namespace

struct DepthSamplingView final : GPUView
{
    DepthSamplingView()
        : sceneVertices(buildScene())
        , sceneBuffer(Device::shared().makeBuffer(
              sceneVertices.data(), sceneVertices.size() * sizeof(SceneVertex)))
        , puffBuffer(Device::shared().makeBuffer(
              nullptr, puffVertexCount * sizeof(PuffVertex)))
    {
        // One sample. The edge a hard particle cuts is the thing being
        // demonstrated, and multisampling would feather it into a weak version
        // of the soft one; a texture target is single-sampled anyway, so this
        // also keeps one pipeline per program instead of two.
        setSampleCount(1);

        // The panels that draw the scene again need a depth test of their own -
        // the off-screen target's buffer is being *read* by then, not attached.
        setDepth(true);

        prepareShaders();
        setContinuous(true);
    }

    void prepareShaders()
    {
        auto scenePipeline = RenderPipelineDescriptor {};
        scenePipeline.sampleCount = sampleCount();
        scenePipeline.depth = true;
        scenePipeline.depthWrite = true;

        sceneToDrawable.prepare(scenePipeline);

        scenePipeline.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
        sceneToTarget.prepare(scenePipeline);

        // A full-screen panel writes over whatever is there and takes no part
        // in the depth test - but it still has to say the attachment is there,
        // which every pipeline drawing into a pass with one does.
        auto panelPipeline = RenderPipelineDescriptor {};
        panelPipeline.sampleCount = sampleCount();
        panelPipeline.depth = true;
        panelPipeline.depthCompare = CompareFunction::Always;
        panelPipeline.depthWrite = false;

        colorPanel.prepare(panelPipeline);
        depthPanel.prepare(panelPipeline);

        colorPanel.setVertices(fullScreenQuad);
        depthPanel.setVertices(fullScreenQuad);

        // The R32Float target carries no depth buffer, so its pipeline must not
        // claim one.
        auto copyPipeline = RenderPipelineDescriptor {};
        copyPipeline.sampleCount = 1;
        copyPipeline.colorFormat = pixelFormatFor(TextureFormat::R32Float);

        depthCopy.prepare(copyPipeline);
        depthCopy.setVertices(fullScreenQuad);

        // Tested against the scene, writing none of its own: two puffs
        // overlapping have to blend rather than the nearer one hiding the
        // further, and neither may disturb the buffer the next panel reads.
        auto puffPipeline = RenderPipelineDescriptor {};
        puffPipeline.sampleCount = sampleCount();
        puffPipeline.depth = true;
        puffPipeline.depthCompare = CompareFunction::LessEqual;
        puffPipeline.depthWrite = false;
        puffPipeline.blendMode = BlendMode::AlphaBlend;

        hardPuffs.prepare(puffPipeline);
        softPuffs.prepare(puffPipeline);
    }

    // The off-screen target is one panel wide and the full height, because
    // every panel is: a fragment in the soft panel looks its own clip position
    // up in this texture, and a target of another shape would send it to
    // another texel.
    void resizeTargets(int width, int height)
    {
        if (width == targetWidth && height == targetHeight)
            return;

        targetWidth = width;
        targetHeight = height;

        sceneTarget.emplace(
            Device::shared().makeTexture(describeSceneTarget(width, height)));
        copyTarget.emplace(
            Device::shared().makeTexture(describeDepthCopy(width, height)));

        colorPanel.sceneColor = *sceneTarget;
        depthPanel.sceneDepth = *sceneTarget;
        depthCopy.sceneDepth = *sceneTarget;
        softPuffs.sceneDepth = *sceneTarget;

        depthValues.assign((std::size_t) (width * height), 0.f);
    }

    void update(Threads::FrameTime time) override
    {
        spin += radiansPerSecond * (float) time.delta;
    }

    Camera cameraFor(float aspect) const
    {
        auto eye = Vec3 {
            std::sin(spin) * orbitRadius, orbitHeight, std::cos(spin) * orbitRadius};

        constexpr auto target = Vec3 {0.f, 0.65f, 0.f};
        constexpr auto worldUp = Vec3 {0.f, 1.f, 0.f};

        auto forward = normalize(target - eye);
        auto right = normalize(cross(forward, worldUp));

        auto projection =
            perspective(aspect, radians(fieldOfView), nearPlane, farPlane);

        return {multiply(projection, lookAt(eye, target, worldUp)),
                right,
                cross(right, forward)};
    }

    // Six quads facing the camera, rebuilt every frame because the camera
    // turns. A billboard that kept last frame's basis would lean out of the
    // screen, and the fade would then be measuring the wrong distance.
    void updatePuffBuffer(const Camera& camera)
    {
        auto vertices = std::vector<PuffVertex> {};
        vertices.reserve(puffVertexCount);

        constexpr Vec2 corners[6] = {{-1.f, -1.f},
                                     {1.f, -1.f},
                                     {1.f, 1.f},
                                     {-1.f, -1.f},
                                     {1.f, 1.f},
                                     {-1.f, 1.f}};

        for (const auto& puff: puffs)
            for (auto corner: corners)
            {
                auto offset = camera.right * (corner.x * puff.radius)
                              + camera.up * (corner.y * puff.radius);

                vertices.push_back({puff.centre + offset, corner});
            }

        puffBuffer.update(vertices.data(), vertices.size() * sizeof(PuffVertex));
    }

    void drawScene(RenderPass& pass, SceneShader& shader, const Camera& camera)
    {
        shader.viewProjection = camera.viewProjection;
        shader.lightDirection = Array {0.42f, 0.82f, 0.39f};

        pass.bind(shader, sceneBuffer);

        // From the vector rather than from the constant beside it: the buffer
        // holds what buildScene made, and a draw past the end of it is a read
        // of whatever follows.
        pass.draw((int) sceneVertices.size());
    }

    template <typename Shader>
    void drawPuffs(RenderPass& pass, Shader& shader, const Camera& camera)
    {
        shader.viewProjection = camera.viewProjection;
        shader.tint = {0.88f, 0.86f, 0.82f};
        shader.opacity = 0.72f;

        pass.bind(shader, puffBuffer);
        pass.draw(puffVertexCount);
    }

    void renderPanel(RenderPass& pass, int panel, const Camera& camera)
    {
        if (panel == 0)
        {
            pass.draw(colorPanel);
            return;
        }

        if (panel == 1)
        {
            pass.draw(depthPanel);
            return;
        }

        drawScene(pass, sceneToDrawable, camera);

        if (panel == 2)
            drawPuffs(pass, hardPuffs, camera);
        else
            drawPuffs(pass, softPuffs, camera);
    }

    void render(Frame& frame) override
    {
        auto bounds = getLocalBounds();

        if (bounds.w <= 0.f || bounds.h <= 0.f)
            return;

        // Needed before there is a pass to ask, and safe to derive: while an
        // off-screen render runs, backingScale() reports the scale that render
        // was asked for, so this is the same number the pass below will give.
        auto scale = backingScale();
        auto pixelWidth = (int) std::lround(bounds.w * (double) scale);
        auto pixelHeight = (int) std::lround(bounds.h * (double) scale);
        auto panelWidth = pixelWidth / panelCount;

        if (panelWidth <= 0 || pixelHeight <= 0)
            return;

        resizeTargets(panelWidth, pixelHeight);

        if (!sceneTarget->hasSampleableDepth())
            return;

        auto camera = cameraFor((float) panelWidth / (float) pixelHeight);
        updatePuffBuffer(camera);

        renderSceneTarget(frame, camera);
        renderDepthCopy(frame);

        if (readBackDepth)
        {
            // A pass recorded on a frame has not reached the GPU until the
            // frame ends, so without this the read would come back with the
            // frame before it - and on the first frame, with nothing.
            frame.flush();
            copyTarget->read(depthValues.data());
        }

        renderPanels(frame, camera, panelWidth);
    }

    // The pass everything after it reads. It keeps its depth rather than
    // discarding it, which is what makes the values still be there - on a
    // tile-based GPU the buffer would otherwise never leave tile memory.
    void renderSceneTarget(Frame& frame, const Camera& camera)
    {
        auto descriptor = RenderPassDescriptor {};
        descriptor.clearColor = backgroundColor;
        descriptor.depthAction = DepthAction::Keep;
        descriptor.label = "scene";

        auto pass = frame.beginPass(*sceneTarget, descriptor);
        drawScene(pass, sceneToTarget, camera);
    }

    // The depth out into a colour format, which is the shape a depth copy has
    // and the reason R32Float exists beside this. Nothing on screen reads it;
    // --check reads it back with Texture::read.
    void renderDepthCopy(Frame& frame)
    {
        auto descriptor = RenderPassDescriptor {};
        descriptor.clearColor = Graphics::Color::black();
        descriptor.label = "depth copy";

        auto pass = frame.beginPass(*copyTarget, descriptor);
        pass.draw(depthCopy);
    }

    void renderPanels(Frame& frame, const Camera& camera, int panelWidth)
    {
        auto descriptor = RenderPassDescriptor {};
        descriptor.clearColor = backgroundColor;

        auto pass = frame.beginPass(descriptor);
        auto pixelHeight = (float) pass.targetHeight();

        if (pass.targetWidth() <= 0 || pixelHeight <= 0.f)
            return;

        for (auto panel = 0; panel < panelCount; ++panel)
        {
            // Exactly the target's width rather than an even division of the
            // drawable's, so a fragment's clip position lands on the texel the
            // off-screen pass wrote. Up to three columns at the right edge are
            // left as background, which is the price of that.
            auto left = (float) (panel * panelWidth);

            pass.setViewport({left, 0.f, (float) panelWidth, pixelHeight});
            renderPanel(pass, panel, camera);
        }

        pass.clearViewport();
    }

    static constexpr float radiansPerSecond = 0.35f;
    static constexpr float orbitRadius = 6.4f;
    static constexpr float orbitHeight = 2.55f;

    // What --check reads back, and the reason the copy pass is not conditional:
    // the frame it measures has to be the frame the window would have drawn.
    bool readBackDepth = false;
    std::vector<float> depthValues;

    int targetWidth = 0;
    int targetHeight = 0;

    std::vector<SceneVertex> sceneVertices;
    Buffer sceneBuffer;
    Buffer puffBuffer;

    std::optional<Texture> sceneTarget;
    std::optional<Texture> copyTarget;

    SceneShader sceneToTarget;
    SceneShader sceneToDrawable;
    ColorPanelShader colorPanel;
    DepthPanelShader depthPanel;
    DepthCopyShader depthCopy;
    PuffShader hardPuffs;
    SoftPuffShader softPuffs;

    // A quarter turn in, which puts a puff in front of the near box, one behind
    // the far one and one against the column. Fixed rather than animated in
    // --check, where nothing drives update().
    float spin = radians(38.f);
};

// Drawn over the GPU output through the platform 2D pipeline, the same way the
// Blending example labels its panels.
struct LabelStripView final : Graphics::View
{
    void paint(Graphics::Context& g) override
    {
        const char* names[panelCount] = {
            "colour", "depth", "hard particles", "soft particles"};

        const char* notes[panelCount] = {"the target's colour plane",
                                         "sampled with depthTexture()",
                                         "depth test only",
                                         "depth test + depth read"};

        constexpr auto nameHalfCharWidth = 4.5f;
        constexpr auto noteHalfCharWidth = 3.3f;
        constexpr auto topBaselineY = 24.f;

        auto bounds = getLocalBounds();
        auto panelWidth = bounds.w / (float) panelCount;
        auto bottomBaselineY = bounds.h - 18.f;

        g.setColor(Graphics::Color::white());

        for (auto i = 0; i < panelCount; ++i)
        {
            auto centreX = panelWidth * ((float) i + 0.5f);
            auto nameWidth = (float) std::strlen(names[i]) * nameHalfCharWidth * 2.f;
            auto noteWidth = (float) std::strlen(notes[i]) * noteHalfCharWidth * 2.f;

            g.drawText(std::string {names[i]},
                       {centreX - nameWidth * 0.5f, topBaselineY},
                       nameFont);
            g.drawText(std::string {notes[i]},
                       {centreX - noteWidth * 0.5f, bottomBaselineY},
                       noteFont);
        }
    }

    Graphics::Font nameFont {
        Graphics::FontOptions().withName("Menlo").withSize(15.f)};
    Graphics::Font noteFont {
        Graphics::FontOptions().withName("Menlo").withSize(11.f)};
};

struct RootView final : Graphics::View
{
    void resized() override
    {
        for (auto* child: getSubviews())
            child->setBounds(getLocalBounds());
    }
};

struct DepthSamplingApp
{
    DepthSamplingApp()
    {
        root.addSubview(scene);
        root.addSubview(labels);
        window.setContentView(root);
    }

    RootView root;
    DepthSamplingView scene;
    LabelStripView labels;
    Graphics::Window window {windowOptions()};
};

namespace
{
constexpr auto checkPanelSize = 200;
constexpr auto gridWidth = 38;
constexpr auto gridHeight = 19;

float luminanceOf(const Graphics::Color& color)
{
    return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
}

float luminanceAt(const Graphics::Image& image, int panel, int x, int y)
{
    return luminanceOf(image.at(panel * checkPanelSize + x, y));
}

// A whole cell of the grid rather than one pixel out of it, so a puff's edge
// lands somewhere on the ramp instead of falling between two samples.
float cellLuminance(const Graphics::Image& image, int panel, int column, int row)
{
    auto left = column * checkPanelSize / gridWidth;
    auto right = (column + 1) * checkPanelSize / gridWidth;
    auto top = row * checkPanelSize / gridHeight;
    auto bottom = (row + 1) * checkPanelSize / gridHeight;

    auto total = 0.f;

    for (auto y = top; y < bottom; ++y)
        for (auto x = left; x < right; ++x)
            total += luminanceAt(image, panel, x, y);

    return total / (float) ((right - left) * (bottom - top));
}

// What the same value would have come back as out of an eight-bit texture.
float quantiseToByte(float value)
{
    return std::round(std::clamp(value, 0.f, 1.f) * 255.f) / 255.f;
}

// And out of a half float: ten stored mantissa bits below the leading one, so
// the step is 2^(exponent - 11) at whatever exponent the value has. Computed
// rather than converted through a half type, there being no portable one.
float quantiseToHalf(float value)
{
    if (value == 0.f)
        return 0.f;

    auto exponent = 0;
    std::frexp(value, &exponent);

    auto step = std::ldexp(1.f, exponent - 11);
    return std::round(value / step) * step;
}

// One step of a format, in world units at the depth it is measured at. This is
// the number the precision argument is actually about: a fade thinner than one
// step of the format the depth was copied into cannot be expressed at all,
// whatever the fade distance says.
float stepInUnits(float depth, float depthStep)
{
    return std::abs(linearDistance(depth + depthStep) - linearDistance(depth));
}

// Two panels side by side, which is the comparison rather than an economy: the
// hard edge and the soft one are the same puffs a column apart.
void printPanels(const Graphics::Image& image,
                 int leftPanel,
                 int rightPanel,
                 const char* leftName,
                 const char* rightName)
{
    const char* ramp = " .:-=+*#%@";

    std::printf("\n%-*s   %s\n", gridWidth, leftName, rightName);

    for (auto row = 0; row < gridHeight; ++row)
    {
        for (auto panel: {leftPanel, rightPanel})
        {
            for (auto column = 0; column < gridWidth; ++column)
            {
                auto level = cellLuminance(image, panel, column, row);
                auto step = (int) (level * 9.999f);

                std::printf("%c", ramp[std::clamp(step, 0, 9)]);
            }

            if (panel == leftPanel)
                std::printf("   ");
        }

        std::printf("\n");
    }
}

// One frame, rendered with no window, measured three ways: that the target
// reports what it was asked for, what the depth survived as in the format the
// copy landed in, and that the soft panel is genuinely darker than the hard one
// where a puff meets the floor.
//
// That last one is what stands in for looking at the window. It locates itself
// rather than trusting a hand-picked pixel: the fade can only ever remove
// alpha, and the puffs are lighter than the floor, so the pixel where the soft
// panel falls furthest below the hard one *is* the intersection - and the depth
// read out of the R32Float copy there is the surface it was fading into.
int runCheck()
{
    auto view = DepthSamplingView {};
    view.readBackDepth = true;
    view.setBounds(
        {0.f, 0.f, (float) (checkPanelSize * panelCount), (float) checkPanelSize});

    auto image = view.renderToImage(1.f);

    if (!image.isValid())
    {
        std::printf("no GPU device\n");
        return 1;
    }

    std::printf("target %d x %d   hasSampleableDepth %s   near %.2f  far %.2f\n",
                view.targetWidth,
                view.targetHeight,
                view.sceneTarget->hasSampleableDepth() ? "yes" : "no",
                (double) nearPlane,
                (double) farPlane);

    if (!view.sceneTarget->hasSampleableDepth())
        return 1;

    auto worstX = 0;
    auto worstY = 0;
    auto worstDrop = 0.f;
    auto dissolved = 0;

    for (auto y = 0; y < checkPanelSize; ++y)
        for (auto x = 0; x < checkPanelSize; ++x)
        {
            auto drop = luminanceAt(image, 2, x, y) - luminanceAt(image, 3, x, y);

            if (drop > 0.02f)
                ++dissolved;

            if (drop > worstDrop)
            {
                worstDrop = drop;
                worstX = x;
                worstY = y;
            }
        }

    printPanels(image,
                0,
                1,
                "colour - the target's colour plane",
                "depth - sampled with depthTexture()");
    printPanels(image,
                2,
                3,
                "hard particles - depth test only",
                "soft particles - depth test + read");

    auto minDepth = 1.f;
    auto maxDepth = 0.f;

    for (auto value: view.depthValues)
    {
        minDepth = std::min(minDepth, value);
        maxDepth = std::max(maxDepth, value);
    }

    auto probe =
        view.depthValues[(std::size_t) (worstY * view.targetWidth + worstX)];

    auto asByte = quantiseToByte(probe);
    auto asHalf = quantiseToHalf(probe);

    std::printf("\nR32Float depth copy   %d x %d texels, read with Texture::read\n",
                view.targetWidth,
                view.targetHeight);
    std::printf("  min %.6f  (%.3f units, the nearest geometry)\n",
                (double) minDepth,
                (double) linearDistance(minDepth));
    std::printf("  max %.6f  (%.3f units, the far plane where nothing drew)\n",
                (double) maxDepth,
                (double) linearDistance(maxDepth));

    std::printf("\nthe scene behind the puff at (%d, %d)\n", worstX, worstY);
    std::printf("  as read   R32Float  %.6f  ->  %8.4f units\n",
                (double) probe,
                (double) linearDistance(probe));
    std::printf("  as 8 bits           %.6f  ->  %8.4f units  (%.4f off)\n",
                (double) asByte,
                (double) linearDistance(asByte),
                (double) std::abs(linearDistance(asByte) - linearDistance(probe)));
    std::printf("  as a half float     %.6f  ->  %8.4f units  (%.4f off)\n",
                (double) asHalf,
                (double) linearDistance(asHalf),
                (double) std::abs(linearDistance(asHalf) - linearDistance(probe)));

    auto exponent = 0;
    std::frexp(probe, &exponent);

    auto floatStep = stepInUnits(probe, std::ldexp(1.f, exponent - 24));
    auto halfStep = stepInUnits(probe, std::ldexp(1.f, exponent - 11));
    auto byteStep = stepInUnits(probe, 1.f / 255.f);

    std::printf("  one step of the format here, in world units\n");
    std::printf("    R32Float %.6f   half float %.4f   8 bits %.4f\n",
                (double) floatStep,
                (double) halfStep,
                (double) byteStep);
    std::printf("  steps across the %.2f-unit fade\n", (double) fadeDistance);
    std::printf("    R32Float %d   half float %d   8 bits %d\n",
                (int) (fadeDistance / floatStep),
                (int) (fadeDistance / halfStep),
                (int) (fadeDistance / byteStep));

    std::printf("\nhard vs soft\n");
    std::printf("  luminance at (%d, %d)   hard %.4f   soft %.4f   drop %.4f\n",
                worstX,
                worstY,
                (double) luminanceAt(image, 2, worstX, worstY),
                (double) luminanceAt(image, 3, worstX, worstY),
                (double) worstDrop);
    std::printf("  %.1f%% of the panel the fade dissolved\n",
                100.f * (float) dissolved
                    / (float) (checkPanelSize * checkPanelSize));

    if (worstDrop <= 0.02f)
    {
        std::printf("\nthe soft panel does not differ from the hard one\n");
        return 1;
    }

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--check") == 0)
        return runCheck();

    return eacp::Apps::run<DepthSamplingApp>();
}
