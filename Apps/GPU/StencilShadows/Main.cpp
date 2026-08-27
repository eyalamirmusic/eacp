#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// Stencil shadow volumes: the algorithm the stencil buffer exists for, and the
// one a Doom-3-era renderer is built out of.
//
// A shadow is not painted here. It is *counted*. The cube's silhouette is
// extruded away from the light into a closed solid - the volume of space the
// cube hides - and that solid is rendered into the stencil buffer alone, with
// the two facings counting in opposite directions. A pixel the volume covers an
// odd number of times over is inside it, and inside it means in shadow. The
// lighting pass then draws only where the count came back to zero.
//
// Three panels, one scene, so the mechanism is visible rather than described:
//
//   no stencil       the lit pass with nothing masking it - no shadow at all
//   shadow volumes   the same, with the extruded solid drawn translucently, so
//                    what is being counted can be seen
//   stencil shadows  the volume drawn to the stencil buffer only, which is the
//                    result the algorithm is for
//
// The counting is **depth-fail** (Carmack's reverse): a volume fragment that
// *fails* the depth test - one lying at or behind the surface already drawn
// there - increments on back faces and decrements on front faces. Counting
// depth *passes* instead is a line shorter and breaks the moment the camera
// enters a shadow, which is why every engine that ships this counts failures.
//
// What eacp is asked for, and all of it is Phase 0 work:
//   GPUView::setStencil                     the attachment
//   RenderPipelineDescriptor::stencilFront  per-facing ops, in one pass over
//                            /stencilBack   the volume rather than two
//   StencilOp::Increment/DecrementWrap      so an over- and an under-count still
//                                           cancel past 255
//   CompareFunction::Equal + reference 0    the lighting pass's mask
//   RenderPass::bind(program, vertices)     four programs over two app-owned
//                                           buffers the app updates in place
//
// Run with --check to render one frame headlessly and print a luminance grid,
// which is how the shadow is confirmed to fall where it should without anyone
// having to look at a window.

using namespace eacp;
using namespace GPU;

namespace
{
constexpr auto panelCount = 3;
constexpr auto pi = 3.14159265358979f;

// Far enough that the extruded cap clears any surface the shadow could land on,
// and well inside the far plane - depth-fail needs the cap rasterized, so a
// volume running past the far plane would have its count cut off.
constexpr auto extrusionLength = 25.f;

float radians(float degrees)
{
    return degrees * (pi / 180.f);
}

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
// builds in the shader, written out here because this scene sends one finished
// matrix rather than the scalars that shader takes.
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

struct SceneVertex
{
    Vec3 position;
    Vec3 normal;
};

struct VolumeVertex
{
    Vec3 position;
};

// The occluder, as eight corners and six quads wound counter-clockwise seen
// from outside - eacp's front-face convention, which is what lets the volume's
// two facings be told apart by the rasterizer instead of by a flag.
constexpr Vec3 cubeCorners[8] = {
    {-1.f, -1.f, -1.f},
    {1.f, -1.f, -1.f},
    {1.f, 1.f, -1.f},
    {-1.f, 1.f, -1.f},
    {-1.f, -1.f, 1.f},
    {1.f, -1.f, 1.f},
    {1.f, 1.f, 1.f},
    {-1.f, 1.f, 1.f},
};

constexpr int cubeFaces[6][4] = {
    {4, 5, 6, 7}, // +z
    {1, 0, 3, 2}, // -z
    {5, 1, 2, 6}, // +x
    {0, 4, 7, 3}, // -x
    {3, 7, 6, 2}, // +y
    {0, 1, 5, 4}, // -y
};

// One undirected edge of the occluder, held as the directed edge it is in each
// of the two faces that meet along it. A silhouette edge is one whose faces
// disagree about facing the light, and the side wall hung from it has to be
// wound from the *light-facing* face's direction or the volume comes out inside
// out - which does not draw wrongly, it counts backwards, and the shadow lands
// everywhere except where the occluder is.
struct CubeEdge
{
    int faceA = 0;
    int startA = 0;
    int endA = 0;
    int faceB = 0;
    int startB = 0;
    int endB = 0;
};

std::vector<CubeEdge> buildCubeEdges()
{
    auto edges = std::vector<CubeEdge> {};

    for (auto face = 0; face < 6; ++face)
        for (auto corner = 0; corner < 4; ++corner)
        {
            auto start = cubeFaces[face][corner];
            auto end = cubeFaces[face][(corner + 1) % 4];

            auto existing =
                std::find_if(edges.begin(),
                             edges.end(),
                             [&](const CubeEdge& edge)
                             { return edge.startA == end && edge.endA == start; });

            if (existing != edges.end())
            {
                existing->faceB = face;
                existing->startB = start;
                existing->endB = end;
                continue;
            }

            edges.push_back({face, start, end, -1, 0, 0});
        }

    return edges;
}

struct Camera
{
    Mat4 viewProjection {};
};

// The light sits close to the occluder and only a little above it, which is
// what makes the shadow long and clearly larger than the cube - a point light
// far away casts a shadow the size of what casts it, and the mechanism is
// easier to read when the shape on the ground is plainly not the cube.
constexpr auto lightPosition = Vec3 {1.8f, 2.4f, 1.3f};
constexpr auto groundHeight = -1.25f;
constexpr auto groundExtent = 6.f;
constexpr auto cubeCentre = Vec3 {0.f, 0.45f, 0.f};
constexpr auto cubeHalfSize = 0.62f;

// Ambient is deliberately low. What the stencil removes is the *diffuse* term,
// so a scene lit mostly by ambient has a shadow barely darker than the floor
// around it, and the panel that is the whole point looks like the panel that
// is the control.
constexpr auto ambientLevel = 0.26f;
constexpr auto diffuseLevel = 1.5f;

// The ground first, then the cube, in one buffer the view updates in place -
// which is the shape RenderPass::bind exists for: one app-owned buffer drawn as
// sub-ranges that differ by a uniform.
constexpr auto groundVertexCount = 6;
constexpr auto cubeVertexCount = 36;
constexpr auto sceneVertexCount = groundVertexCount + cubeVertexCount;

// Front cap and back cap are at most every face of the occluder, and the side
// walls at most every edge - the worst case, which never all happens at once
// but is what the buffer has to hold.
constexpr auto maxVolumeVertices = 6 * 6 + 6 * 6 + 12 * 6;

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1080;
    options.height = 400;
    options.minWidth = 600;
    options.minHeight = 240;
    options.title = "eacp GPU - Stencil Shadows";
    return options;
}

// Lit by a point light in the fragment stage, so a ground plane of two
// triangles still shades smoothly. The lighting pass and the ambient pass are
// the same shader with different uniforms and different pipelines.
struct SceneShader final : ShaderProgram
{
    SceneShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&SceneVertex::position);
        auto normal = vertexInput(&SceneVertex::normal);

        setPosition(viewProjection * float4(position, 1.f));

        auto worldPosition = varying(position);
        auto worldNormal = normalize(varying(normal));
        auto toLight = normalize(lightPoint - worldPosition);
        auto lambert = max(dot(worldNormal, toLight), 0.f);

        setFragment(float4(baseColor * (ambient + lambert * diffuse), 1.f));
    }

    Uniform<Float4x4> viewProjection;
    Uniform<Float3> lightPoint;
    Uniform<Float3> baseColor;
    Uniform<Float> ambient;
    Uniform<Float> diffuse;

    EACP_SHADER(viewProjection, lightPoint, baseColor, ambient, diffuse)
};

// The volume carries no normal and no lighting: its whole job is to be
// rasterized so the stencil ops fire. The colour matters in one panel only.
struct VolumeShader final : ShaderProgram
{
    VolumeShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&VolumeVertex::position);

        setPosition(viewProjection * float4(position, 1.f));
        setFragment(color);
    }

    Uniform<Float4x4> viewProjection;
    Uniform<Float4> color;

    EACP_SHADER(viewProjection, color)
};
} // namespace

struct StencilShadowsView final : GPUView
{
    StencilShadowsView()
        : edges(buildCubeEdges())
        , sceneBuffer(Device::shared().makeBuffer(
              nullptr, sceneVertexCount * sizeof(SceneVertex)))
        , volumeBuffer(Device::shared().makeBuffer(
              nullptr, maxVolumeVertices * sizeof(VolumeVertex)))
    {
        // Multisampling would feather the shadow's edge, and the edge is the
        // thing being demonstrated. One sample keeps the boundary exactly where
        // the count changed.
        setSampleCount(1);

        // The whole point, and it brings the depth buffer with it: the two
        // planes are one attachment, and depth-fail counting needs both.
        setStencil(true);

        prepareScenePipelines();
        prepareVolumePipelines();

        setContinuous(true);
    }

    // Ambient fills the depth buffer the volume pass then reads; lighting adds
    // on top of it, testing depth without writing any, and drawing only where
    // the stencil count came back to zero.
    void prepareScenePipelines()
    {
        auto ambientPipeline = RenderPipelineDescriptor {};
        ambientPipeline.sampleCount = sampleCount();
        ambientPipeline.depth = true;
        ambientPipeline.depthWrite = true;

        // Set even though nothing here tests the stencil: the attachment's
        // format is part of what a pipeline is compiled against, and a view
        // carrying the plane needs every pipeline drawing into it to say so.
        ambientPipeline.stencil = true;
        ambientPipeline.cullMode = CullMode::Back;

        ambient.prepare(ambientPipeline);

        auto litPipeline = RenderPipelineDescriptor {};
        litPipeline.sampleCount = sampleCount();
        litPipeline.depth = true;
        litPipeline.depthCompare = CompareFunction::LessEqual;
        litPipeline.depthWrite = false;
        litPipeline.blendMode = BlendMode::Additive;
        litPipeline.cullMode = CullMode::Back;
        litPipeline.stencil = true;

        // The mask. Zero is "no volume covered this pixel", and the two faces
        // read alike because a surface being lit has nothing to do with which
        // way the volume around it was wound.
        litPipeline.stencilFront.compare = CompareFunction::Equal;
        litPipeline.stencilBack.compare = CompareFunction::Equal;

        lit.prepare(litPipeline);
    }

    // Depth-fail counting, both facings in one pass over the volume - which is
    // what per-face stencil state buys, and what a one-face-at-a-time API costs
    // a second pass over the same geometry.
    RenderPipelineDescriptor volumeDescriptor(BlendMode blendMode) const
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.sampleCount = sampleCount();
        descriptor.depth = true;

        // Fragments at or behind what is already there are the ones that count,
        // and none of them may disturb the depth the lighting pass is about to
        // test against.
        descriptor.depthCompare = CompareFunction::Less;
        descriptor.depthWrite = false;

        // Both facings rasterized, which is the whole reason the two stencil
        // faces exist.
        descriptor.cullMode = CullMode::None;
        descriptor.blendMode = blendMode;

        descriptor.stencil = true;
        descriptor.stencilBack.depthFail = StencilOp::IncrementWrap;
        descriptor.stencilFront.depthFail = StencilOp::DecrementWrap;

        return descriptor;
    }

    void prepareVolumePipelines()
    {
        visibleVolume.prepare(volumeDescriptor(BlendMode::AlphaBlend));

        // The invisible one. eacp has no colour write mask, so the volume is
        // made to write nothing by drawing it additively at zero alpha:
        // Additive is (SRC_ALPHA, ONE), so a fragment with no alpha contributes
        // nothing and the destination survives untouched. The stencil ops still
        // fire, which is all this pass is for.
        //
        // A real renderer wants the mask - Doom 3 sets glColorMask here - and
        // this is the workaround until eacp grows one.
        hiddenVolume.prepare(volumeDescriptor(BlendMode::Additive));
    }

    void update(Threads::FrameTime time) override
    {
        spin += radiansPerSecond * (float) time.delta;
    }

    // The occluder's corners in world space. Rebuilt every frame because the
    // cube turns, and the volume has to be built from the same eight points the
    // cube is drawn from or its silhouette would belong to another pose.
    void updateCubeCorners()
    {
        auto cosSpin = std::cos(spin);
        auto sinSpin = std::sin(spin);
        auto tilt = radians(24.f);
        auto cosTilt = std::cos(tilt);
        auto sinTilt = std::sin(tilt);

        for (auto i = 0; i < 8; ++i)
        {
            auto local = cubeCorners[i] * cubeHalfSize;

            auto spun = Vec3 {local.x * cosSpin + local.z * sinSpin,
                              local.y,
                              -local.x * sinSpin + local.z * cosSpin};

            auto tilted = Vec3 {spun.x,
                                spun.y * cosTilt - spun.z * sinTilt,
                                spun.y * sinTilt + spun.z * cosTilt};

            corners[i] = tilted + cubeCentre;
        }

        for (auto face = 0; face < 6; ++face)
        {
            auto a = corners[cubeFaces[face][0]];
            auto b = corners[cubeFaces[face][1]];
            auto c = corners[cubeFaces[face][2]];

            faceNormals[face] = normalize(cross(b - a, c - a));

            // Facing is measured against the light's direction from the face,
            // not against the camera: a shadow does not care where it is
            // watched from.
            faceFacesLight[face] = dot(faceNormals[face], lightPosition - a) > 0.f;
        }
    }

    void appendSceneQuad(std::vector<SceneVertex>& out,
                         Vec3 a,
                         Vec3 b,
                         Vec3 c,
                         Vec3 d,
                         Vec3 normal) const
    {
        out.push_back({a, normal});
        out.push_back({b, normal});
        out.push_back({c, normal});
        out.push_back({a, normal});
        out.push_back({c, normal});
        out.push_back({d, normal});
    }

    void updateSceneBuffer()
    {
        auto vertices = std::vector<SceneVertex> {};
        vertices.reserve(sceneVertexCount);

        constexpr auto up = Vec3 {0.f, 1.f, 0.f};

        appendSceneQuad(vertices,
                        {-groundExtent, groundHeight, groundExtent},
                        {groundExtent, groundHeight, groundExtent},
                        {groundExtent, groundHeight, -groundExtent},
                        {-groundExtent, groundHeight, -groundExtent},
                        up);

        for (auto face = 0; face < 6; ++face)
            appendSceneQuad(vertices,
                            corners[cubeFaces[face][0]],
                            corners[cubeFaces[face][1]],
                            corners[cubeFaces[face][2]],
                            corners[cubeFaces[face][3]],
                            faceNormals[face]);

        sceneBuffer.update(vertices.data(), vertices.size() * sizeof(SceneVertex));
    }

    Vec3 extrude(Vec3 point) const
    {
        return point + normalize(point - lightPosition) * extrusionLength;
    }

    void appendVolumeTriangle(std::vector<VolumeVertex>& out,
                              Vec3 a,
                              Vec3 b,
                              Vec3 c) const
    {
        out.push_back({a});
        out.push_back({b});
        out.push_back({c});
    }

    // The closed solid the cube hides: the faces that see the light as its near
    // cap, the same faces' opposites pushed away as its far cap, and a wall
    // hung from every silhouette edge in between.
    //
    // Both caps are what depth-fail needs and depth-pass does not. They are also
    // why a convex occluder is easy: every face is either lit or not, so the
    // silhouette is exactly the edges whose two faces disagree.
    void updateVolumeBuffer()
    {
        auto vertices = std::vector<VolumeVertex> {};
        vertices.reserve(maxVolumeVertices);

        for (auto face = 0; face < 6; ++face)
        {
            auto a = corners[cubeFaces[face][0]];
            auto b = corners[cubeFaces[face][1]];
            auto c = corners[cubeFaces[face][2]];
            auto d = corners[cubeFaces[face][3]];

            if (faceFacesLight[face])
            {
                appendVolumeTriangle(vertices, a, b, c);
                appendVolumeTriangle(vertices, a, c, d);
            }
            else
            {
                // Extruded, and in the same winding: a face already pointing
                // away from the light points out of the far end of the volume.
                appendVolumeTriangle(vertices, extrude(a), extrude(b), extrude(c));
                appendVolumeTriangle(vertices, extrude(a), extrude(c), extrude(d));
            }
        }

        for (const auto& edge: edges)
        {
            if (faceFacesLight[edge.faceA] == faceFacesLight[edge.faceB])
                continue;

            // Taken in the light-facing face's own direction, so the wall's
            // outward side faces out of the volume.
            auto lit = faceFacesLight[edge.faceA];
            auto start = corners[lit ? edge.startA : edge.startB];
            auto end = corners[lit ? edge.endA : edge.endB];

            auto startFar = extrude(start);
            auto endFar = extrude(end);

            appendVolumeTriangle(vertices, start, startFar, endFar);
            appendVolumeTriangle(vertices, start, endFar, end);
        }

        volumeVertexCount = (int) vertices.size();

        if (volumeVertexCount > 0)
            volumeBuffer.update(vertices.data(),
                                vertices.size() * sizeof(VolumeVertex));
    }

    Camera cameraFor(float aspect) const
    {
        auto view = lookAt({0.f, 2.1f, 5.2f}, {0.f, -0.35f, 0.f}, {0.f, 1.f, 0.f});
        auto projection = perspective(aspect, radians(42.f), 0.1f, 100.f);

        return {multiply(projection, view)};
    }

    // The scene twice over: ground, then cube, differing only in their colour,
    // so the second draw restates the uniform block and nothing else. That is
    // what bind() leaves to the caller.
    void drawScene(RenderPass& pass, SceneShader& shader, const Camera& camera) const
    {
        shader.viewProjection = camera.viewProjection;
        shader.lightPoint =
            Array {lightPosition.x, lightPosition.y, lightPosition.z};
        shader.baseColor = Array {0.34f, 0.36f, 0.42f};

        pass.bind(shader, sceneBuffer);
        pass.draw(groundVertexCount, 0);

        shader.baseColor = Array {0.86f, 0.52f, 0.30f};
        pass.setUniforms(shader);
        pass.draw(cubeVertexCount, groundVertexCount);
    }

    void drawVolume(RenderPass& pass,
                    VolumeShader& shader,
                    const Camera& camera) const
    {
        if (volumeVertexCount == 0)
            return;

        shader.viewProjection = camera.viewProjection;

        pass.bind(shader, volumeBuffer);
        pass.draw(volumeVertexCount, 0);
    }

    void renderPanel(RenderPass& pass, int panel, const Camera& camera)
    {
        ambient.ambient = ambientLevel;
        ambient.diffuse = 0.f;
        drawScene(pass, ambient, camera);

        if (panel == 1)
            drawVolume(pass, visibleVolume, camera);
        else if (panel == 2)
            drawVolume(pass, hiddenVolume, camera);

        lit.ambient = 0.f;
        lit.diffuse = diffuseLevel;
        drawScene(pass, lit, camera);
    }

    void render(Frame& frame) override
    {
        auto bounds = getLocalBounds();

        if (bounds.w <= 0.f || bounds.h <= 0.f)
            return;

        updateCubeCorners();
        updateSceneBuffer();
        updateVolumeBuffer();

        visibleVolume.color = Array {0.30f, 0.85f, 0.95f, 0.13f};

        // Nothing at all: the volume that must not be seen writes no colour, so
        // its alpha is what makes it invisible rather than a mask on the
        // pipeline. See prepareVolumePipelines.
        hiddenVolume.color = Array {0.f, 0.f, 0.f, 0.f};

        auto descriptor = RenderPassDescriptor {};
        descriptor.clearColor = Graphics::Color {0.06f, 0.07f, 0.09f};

        // Zero is "covered by no volume", which the lighting pass reads as lit.
        // One clear serves all three panels: their viewports are disjoint, so
        // no panel can see another's count.
        descriptor.clearStencil = 0;

        auto pass = frame.beginPass(descriptor);

        // From the pass, not from the view's bounds times its backing scale.
        // The two agree on screen and part company in a snapshot, which renders
        // at whatever scale renderToImage was given - and a viewport outside the
        // target is a no-op, so the derived numbers would collapse the three
        // panels into one without a word.
        auto pixelWidth = (float) pass.targetWidth();
        auto pixelHeight = (float) pass.targetHeight();

        if (pixelWidth <= 0.f || pixelHeight <= 0.f)
            return;

        for (auto panel = 0; panel < panelCount; ++panel)
        {
            auto left = std::floor(pixelWidth * (float) panel / (float) panelCount);
            auto right =
                std::floor(pixelWidth * (float) (panel + 1) / (float) panelCount);
            auto width = right - left;

            if (width <= 0.f)
                continue;

            pass.setViewport({left, 0.f, width, pixelHeight});
            renderPanel(pass, panel, cameraFor(width / pixelHeight));
        }

        pass.clearViewport();
    }

    static constexpr float radiansPerSecond = 0.55f;

    std::vector<CubeEdge> edges;
    Buffer sceneBuffer;
    Buffer volumeBuffer;

    SceneShader ambient;
    SceneShader lit;
    VolumeShader visibleVolume;
    VolumeShader hiddenVolume;

    Vec3 corners[8] {};
    Vec3 faceNormals[6] {};
    bool faceFacesLight[6] {};

    int volumeVertexCount = 0;
    float spin = 0.f;
};

// Drawn over the GPU output through the platform 2D pipeline, the same way the
// Blending example labels its panels.
struct LabelStripView final : Graphics::View
{
    void paint(Graphics::Context& g) override
    {
        const char* names[panelCount] = {
            "no stencil", "shadow volumes", "stencil shadows"};

        const char* notes[panelCount] = {
            "lit everywhere", "what is being counted", "count == 0 is lit"};

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

struct StencilShadowsApp
{
    StencilShadowsApp()
    {
        root.addSubview(shadows);
        root.addSubview(labels);
        window.setContentView(root);
    }

    RootView root;
    StencilShadowsView shadows;
    LabelStripView labels;
    Graphics::Window window {windowOptions()};
};

namespace
{
float luminanceOf(const Graphics::Color& color)
{
    return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
}

// One frame, rendered with no window, printed as three luminance grids and
// measured. A shadow is a dark region in a lit one, which a terminal can show
// and an arithmetic mean can pin: the panels that count the volume come back
// darker than the one that does not, and by how much is the last two numbers.
//
// This is what stands in for looking at the window, and it is the reason the
// winding of the volume's side walls could be settled rather than assumed - a
// volume wound inside out shadows everything *except* where the occluder is,
// which shows up here as a shadowed fraction near 1 instead of near 0.2.
int printLuminanceGrid()
{
    constexpr auto panelWidth = 44;
    constexpr auto panelHeight = 20;
    constexpr auto width = panelWidth * panelCount;

    auto view = StencilShadowsView {};
    view.setBounds({0.f, 0.f, (float) width, (float) panelHeight});

    auto image = view.renderToImage(1.f);

    if (!image.isValid())
    {
        std::printf("no GPU device\n");
        return 1;
    }

    const char* ramp = " .:-=+*#%@";
    const char* names[panelCount] = {
        "no stencil", "shadow volumes", "stencil shadows"};

    auto means = Array<float, panelCount> {};
    means.fill(0.f);

    for (auto panel = 0; panel < panelCount; ++panel)
    {
        std::printf("\n%s\n", names[panel]);

        for (auto y = 0; y < panelHeight; ++y)
        {
            for (auto x = 0; x < panelWidth; ++x)
            {
                auto luminance = luminanceOf(image.at(panel * panelWidth + x, y));
                auto step = (int) (luminance * 9.999f);

                means[panel] += luminance;
                std::printf("%c", ramp[step < 0 ? 0 : (step > 9 ? 9 : step)]);
            }

            std::printf("\n");
        }

        means[panel] /= (float) (panelWidth * panelHeight);
    }

    // How much of the frame the count darkened, measured against the panel that
    // did no counting. The unlit panel is the control for both of the others.
    auto darkened = 0;

    for (auto y = 0; y < panelHeight; ++y)
        for (auto x = 0; x < panelWidth; ++x)
        {
            auto unshadowed = luminanceOf(image.at(x, y));
            auto shadowed = luminanceOf(image.at(2 * panelWidth + x, y));

            if (unshadowed - shadowed > 0.05f)
                ++darkened;
        }

    std::printf("\nmean luminance   ");

    for (auto panel = 0; panel < panelCount; ++panel)
        std::printf("%-16s %.3f   ", names[panel], means[panel]);

    std::printf("\nshadowed pixels  %.1f%% of the frame\n",
                100.f * (float) darkened / (float) (panelWidth * panelHeight));

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--check") == 0)
        return printLuminanceGrid();

    return eacp::Apps::run<StencilShadowsApp>();
}
