#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Graphics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Cube textures: a sky and a mirror ball, which are the two things six square
// faces sampled by a direction are for.
//
// A 2D texture is read by *where you are on it* - a coordinate. A cube texture
// is read by *where you are pointing* - a direction out of its centre, which
// the hardware resolves into a face and a coordinate on that face. Everything a
// cube map is used for follows from that one difference:
//
//   a sky          the direction is the view ray, so what comes back is what
//                  the camera sees when nothing is in the way
//   a reflection   the direction is the view ray mirrored in a surface, so what
//                  comes back is what the camera sees *via* that surface
//
// Both are in this window at once - a box drawn around the camera carrying the
// first, a sphere in the middle of it carrying the second - and the camera
// orbits and rises so all six faces come past.
//
// **The six faces are uploaded as one block of pixels in +X, -X, +Y, -Y, +Z,
// -Z order, each face square and each face's row 0 at the top.** That is the
// convention TextureDescriptor::cube pins, and it is the thing a reader of this
// example most needs to leave with, so the faces here are built to make a
// mistake in it visible. Each face is one hue - which is which face - times a
// brightness that rises toward u = 1 and toward v = 0, over a grid, with the
// single cell at (u, v) = (0, 0) filled bright. A face in the wrong slot shows
// the wrong colour in that direction; a face flipped or turned within its own
// slot shows the gradient running the wrong way. Neither would be an error
// anywhere: a sky is a sky either way round, which is exactly why it is worth
// being able to see.
//
// Making every texel of a face a scaling of one colour is what lets --check
// read the faces back: scaling a colour does not turn it, so a mip level, a
// bilinear blend and the sphere's smeared reflection all still point at the hue
// they came from.
//
// Run with --check to render frames headlessly and print, first, a map of which
// face each pixel of one oblique view came from, and then, for a camera placed
// on each of the six axes in turn, whether the face ahead, the face in the
// sphere and the direction the gradient runs are the ones the convention says.

using namespace eacp;
using namespace GPU;

namespace
{
constexpr auto pi = 3.14159265358979f;

constexpr float radians(float degrees)
{
    return degrees * (pi / 180.f);
}

// How far the camera orbits at, how big the ball is, how wide the lens is, and
// how far away the sky's walls are - which matters not at all to what is
// sampled (a direction has no length) and only has to clear the far plane.
constexpr auto orbitRadius = 5.f;
constexpr auto sphereRadius = 1.f;
constexpr auto fieldOfView = 50.f;
constexpr auto skyRadius = 40.f;

struct Vec3
{
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    using ShaderValue = Float3;
};

constexpr Vec3 operator-(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

constexpr Vec3 operator*(Vec3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

constexpr float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalize(Vec3 v)
{
    auto length = std::sqrt(dot(v, v));
    return length > 1.0e-6f ? v * (1.f / length) : Vec3 {0.f, 0.f, 1.f};
}

// Column-major, the layout Float4x4 has on both sides of the language boundary.
// The camera here is a position and a target rather than a pair of angles, so
// the matrices are built on the CPU and uploaded, where the Teapot builds its
// own from scalars inside define().
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

// Right-handed with [0, 1] depth, matching ShaderProgram::perspective.
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
    auto back = normalize(eye - target);
    auto right = normalize(cross(up, back));
    auto trueUp = cross(back, right);

    auto out = Mat4 {};

    out[0] = right.x;
    out[1] = trueUp.x;
    out[2] = back.x;
    out[3] = 0.f;
    out[4] = right.y;
    out[5] = trueUp.y;
    out[6] = back.y;
    out[7] = 0.f;
    out[8] = right.z;
    out[9] = trueUp.z;
    out[10] = back.z;
    out[11] = 0.f;
    out[12] = -dot(right, eye);
    out[13] = -dot(trueUp, eye);
    out[14] = -dot(back, eye);
    out[15] = 1.f;

    return out;
}

// The six faces, in the order TextureDescriptor::cube takes them, each with the
// letter --check prints it as and the hue that says which one it is. Six
// directions in RGB rather than six pretty colours: the check tells faces apart
// by which of these a pixel points nearest to, so what matters is that no two
// are close.
struct SkyFace
{
    const char* name;
    char letter;
    Vec3 color;
};

constexpr SkyFace skyFaces[6] = {
    {"+X", 'X', {0.88f, 0.22f, 0.20f}}, // red
    {"-X", 'x', {0.18f, 0.72f, 0.86f}}, // cyan
    {"+Y", 'Y', {0.36f, 0.80f, 0.30f}}, // green
    {"-Y", 'y', {0.85f, 0.30f, 0.80f}}, // magenta
    {"+Z", 'Z', {0.28f, 0.36f, 0.92f}}, // blue
    {"-Z", 'z', {0.92f, 0.72f, 0.18f}}, // amber
};

constexpr auto faceSize = 64;
constexpr auto faceCells = 8;

// The pattern one face carries, as a number its hue is scaled by.
//
// Three marks, each answering a different question about the face:
//
//   the gradient    bright toward u = 1 and toward v = 0, so *any* part of the
//                   face - a corner of the window, a mip level, a smear of it
//                   in the sphere - says which way both axes run
//   the grid        how the face is subdivided, and so how far a lookup moved
//   the origin cell filled flat and bright in the darkest corner, which is the
//                   one place on a face that can be named: (u, v) = (0, 0), the
//                   first texel of the first row of the block uploaded for it
//
// The gradient is what a partial view of the face can be checked against, which
// is why the orientation is carried by it and not by the corner mark alone: a
// camera with a 50 degree lens sees less than half of the face ahead of it, and
// a mark on the edge of that face would be off screen.
float faceBrightness(int x, int y)
{
    constexpr auto cellSize = faceSize / faceCells;

    if (x < cellSize && y < cellSize)
        return 1.f;

    auto u = ((float) x + 0.5f) / (float) faceSize;
    auto v = ((float) y + 0.5f) / (float) faceSize;

    auto shade = (0.28f + 0.62f * u) * (0.45f + 0.55f * (1.f - v));

    // Multiplied in rather than substituted, so the gradient still reads
    // through the grid instead of being cut into strips by it.
    auto onGridLine = (x % cellSize) == 0 || (y % cellSize) == 0;

    return onGridLine ? shade * 0.35f : shade;
}

// RGBA8 is little-endian here: A B G R from the top.
std::uint32_t packColor(Vec3 color, float brightness)
{
    auto channel = [brightness](float value)
    {
        auto scaled = (int) std::lround(value * brightness * 255.f);
        return (std::uint32_t) std::clamp(scaled, 0, 255);
    };

    return channel(color.x) | (channel(color.y) << 8) | (channel(color.z) << 16)
           | 0xff000000u;
}

// All six faces as one block, which is what the upload takes: face after face
// in +X, -X, +Y, -Y, +Z, -Z order, and within a face row after row from the
// top - the same layout every 2D texture here is uploaded in, six times over.
// There is no per-face call and no per-face offset to get wrong, which is the
// whole ergonomics of it: assemble the block, hand it over.
std::vector<std::uint32_t> buildSkyPixels()
{
    auto pixels = std::vector<std::uint32_t> {};
    pixels.reserve(6 * faceSize * faceSize);

    for (const auto& face: skyFaces)
        for (auto y = 0; y < faceSize; ++y)
            for (auto x = 0; x < faceSize; ++x)
                pixels.push_back(packColor(face.color, faceBrightness(x, y)));

    return pixels;
}

Texture makeSkyCube()
{
    auto pixels = buildSkyPixels();

    auto descriptor = TextureDescriptor {};
    descriptor.width = faceSize;
    descriptor.height = faceSize;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.cube = true;

    // A chain per face, built from that face's own pixels. The sky needs it
    // because a 64-texel face stretched across a wide lens is minified at the
    // periphery and magnified ahead, and the sphere needs it far more: the
    // reflection direction sweeps a whole face within a few pixels near the
    // silhouette, and without a chain those pixels alias into noise.
    descriptor.mipmapped = true;

    return Device::shared().makeTexture(descriptor, pixels.data());
}

struct Vertex
{
    Vec3 position;
    Vec3 normal;
};

struct Mesh
{
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

// A UV sphere: rings of latitude, segments of longitude, two triangles per
// quad, wound counter-clockwise seen from outside so the far half can be culled.
//
// The normal of a sphere centred on the origin is its own position scaled, so
// the shader could derive it - it is stored per vertex because that is what
// every mesh that is not a sphere has to do, and this shader is meant to be
// copied into scenes that are not spheres.
Mesh buildSphere(float radius, int rings, int segments)
{
    auto mesh = Mesh {};

    for (auto ring = 0; ring <= rings; ++ring)
    {
        auto polar = pi * (float) ring / (float) rings;

        for (auto segment = 0; segment <= segments; ++segment)
        {
            auto azimuth = 2.f * pi * (float) segment / (float) segments;

            auto normal = Vec3 {std::sin(polar) * std::cos(azimuth),
                                std::cos(polar),
                                std::sin(polar) * std::sin(azimuth)};

            mesh.vertices.push_back({normal * radius, normal});
        }
    }

    for (auto ring = 0; ring < rings; ++ring)
        for (auto segment = 0; segment < segments; ++segment)
        {
            auto a = (std::uint32_t) (ring * (segments + 1) + segment);
            auto b = a + (std::uint32_t) (segments + 1);
            auto c = b + 1;
            auto d = a + 1;

            mesh.indices.insert(mesh.indices.end(), {a, d, c, a, c, b});
        }

    return mesh;
}

struct SkyVertex
{
    Vec3 position;
};

// The eight corners of the box the sky is drawn on. A corner's position is
// also the direction that samples the cube at it, which is the whole trick: the
// box is centred on the camera, so a point on its wall and the direction to
// that point are the same three numbers.
constexpr SkyVertex skyCorners[8] = {
    {{-skyRadius, -skyRadius, -skyRadius}},
    {{skyRadius, -skyRadius, -skyRadius}},
    {{skyRadius, skyRadius, -skyRadius}},
    {{-skyRadius, skyRadius, -skyRadius}},
    {{-skyRadius, -skyRadius, skyRadius}},
    {{skyRadius, -skyRadius, skyRadius}},
    {{skyRadius, skyRadius, skyRadius}},
    {{-skyRadius, skyRadius, skyRadius}},
};

constexpr std::uint32_t skyIndices[36] = {
    4, 5, 6, 4, 6, 7, // +z
    1, 0, 3, 1, 3, 2, // -z
    5, 1, 2, 5, 2, 6, // +x
    0, 4, 7, 0, 7, 3, // -x
    3, 7, 6, 3, 6, 2, // +y
    0, 1, 5, 0, 5, 4, // -y
};

// The sky. The box is moved to the camera in the vertex stage rather than
// standing still in the world, so however far the camera travels the walls stay
// the same distance off - which is what makes it a sky and not a room.
struct SkyShader final : ShaderProgram
{
    SkyShader()
    {
        // Linear so the 64-texel faces do not come apart into squares at this
        // scale, and clamped because that is the only address mode a cube has
        // any use for: a direction cannot run off the edge of a face, and the
        // seam between one face and the next is the hardware's business rather
        // than the sampler's.
        sky.sampling = {TextureFilter::Linear, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&SkyVertex::position);

        setPosition(viewProjection * float4(position + eyePosition, 1.f));

        // The direction is the corner as it stands - forty units out to a
        // wall, seventy to a corner, and never a unit vector. Which face a
        // lookup lands on, and where on it, is decided by the ratios between
        // the three components, so the length is not part of the question.
        setFragment(sample(sky, varying(position)));
    }

    Uniform<Float4x4> viewProjection;
    Uniform<Float3> eyePosition;
    Uniform<TextureCube> sky;

    EACP_SHADER(viewProjection, eyePosition, sky)
};

// The reflection: a perfect mirror, with no lighting term of its own at all.
// That is partly what a chrome ball looks like and partly so that every pixel
// on screen - the sky and the ball alike - is exactly one cube lookup, which is
// what lets --check name the face each of them came from.
struct MirrorShader final : ShaderProgram
{
    MirrorShader()
    {
        sky.sampling = {TextureFilter::Linear, TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&Vertex::position);
        auto normal = vertexInput(&Vertex::normal);

        setPosition(viewProjection * float4(position, 1.f));

        auto surface = varying(position);

        // The one normalize here that is not optional. Interpolating two unit
        // normals across a triangle gives something shorter than either, and
        // reflect's formula - I - 2 (I . N) N - is a mirroring only when N is
        // unit length. A stretched normal here tilts the reflection.
        auto surfaceNormal = normalize(varying(normal));

        // The eye vector is not normalized, and neither is the reflection of
        // it, and both omissions are for the same reason. reflect is linear in
        // its incident argument, so scaling the eye vector scales the result
        // and leaves its *direction* alone - and a direction is all a cube
        // lookup reads. The two square roots a reflection shader is usually
        // written with would not move a single texel.
        auto incident = surface - eyePosition;

        setFragment(sample(sky, reflect(incident, surfaceNormal)));
    }

    Uniform<Float4x4> viewProjection;
    Uniform<Float3> eyePosition;
    Uniform<TextureCube> sky;

    EACP_SHADER(viewProjection, eyePosition, sky)
};

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 900;
    options.height = 600;
    options.minWidth = 480;
    options.minHeight = 320;
    options.title = "eacp GPU - Cube Map";
    return options;
}
} // namespace

struct CubeMapView final : GPUView
{
    // One sample under --check, four in the window. A multisampled edge is a
    // blend of what is on either side of it, and at the sphere's silhouette
    // that is a blend of two faces - a colour belonging to neither, which the
    // face map would have to invent an answer for.
    explicit CubeMapView(int samples = 4)
        : sky(makeSkyCube())
        , sphere(buildSphere(sphereRadius, 24, 48))
    {
        setSampleCount(samples);

        skyShader.setVertices(skyCorners);
        skyShader.setIndices(skyIndices);
        skyShader.sky = sky;
        skyShader.prepare(skyPipeline());

        mirrorShader.setVertices(sphere.vertices.data(),
                                 (int) sphere.vertices.size());
        mirrorShader.setIndices(sphere.indices.data(), (int) sphere.indices.size());
        mirrorShader.sky = sky;
        mirrorShader.prepare(mirrorPipeline());

        lookFrom(orbitEye(0.f), {0.f, 1.f, 0.f});
        setContinuous(true);
    }

    // There is no depth buffer in this scene, and that is worth stating rather
    // than reaching for one out of habit. A ray out of the middle of a box
    // leaves through exactly one wall, so no two walls of the sky can cover the
    // same pixel; and the sphere is convex, so back-face culling leaves exactly
    // one triangle over each pixel it covers. Draw the sky, then the ball on
    // top of it, and there is nothing left for a depth test to decide.
    RenderPipelineDescriptor skyPipeline() const
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.sampleCount = sampleCount();

        return descriptor;
    }

    RenderPipelineDescriptor mirrorPipeline() const
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.sampleCount = sampleCount();

        // What stands in for the depth buffer. Without it the far half of the
        // sphere would paint over the near half wherever it came later in the
        // ring order, which for half the rings is everywhere.
        descriptor.cullMode = CullMode::Back;

        return descriptor;
    }

    void update(Threads::FrameTime time) override
    {
        elapsed += (float) time.delta;
        lookFrom(orbitEye(elapsed), {0.f, 1.f, 0.f});
    }

    // A slow turn around the ball and a slower rise and fall over it. The two
    // periods do not divide one another, so the camera walks the sky instead of
    // retracing one circle of it, and everything comes past in the end - the
    // +Y face overhead and the -Y face underneath included, which a camera that
    // only ever turned would never show.
    static Vec3 orbitEye(float seconds)
    {
        auto yaw = seconds * 0.31f;
        auto pitch = std::sin(seconds * 0.13f) * radians(62.f);

        return {orbitRadius * std::cos(pitch) * std::sin(yaw),
                orbitRadius * std::sin(pitch),
                orbitRadius * std::cos(pitch) * std::cos(yaw)};
    }

    void lookFrom(Vec3 eye, Vec3 up)
    {
        eyePosition = eye;
        upDirection = up;
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.f, 0.f, 0.f}});

        // From the pass rather than from the view's bounds: the two agree on
        // screen and part company in a snapshot, which renders at whatever
        // scale renderToImage was given.
        auto width = (float) pass.targetWidth();
        auto height = (float) pass.targetHeight();

        if (width <= 0.f || height <= 0.f)
            return;

        auto view = lookAt(eyePosition, {0.f, 0.f, 0.f}, upDirection);
        auto projection =
            perspective(width / height, radians(fieldOfView), 0.1f, 200.f);

        auto viewProjection = multiply(projection, view);
        auto eye = Array {eyePosition.x, eyePosition.y, eyePosition.z};

        skyShader.viewProjection = viewProjection;
        skyShader.eyePosition = eye;

        mirrorShader.viewProjection = viewProjection;
        mirrorShader.eyePosition = eye;

        // The sky covers every pixel of the frame, so the clear colour is never
        // seen; the ball goes on top of it.
        pass.draw(skyShader);
        pass.draw(mirrorShader);
    }

    Texture sky;
    Mesh sphere;

    SkyShader skyShader;
    MirrorShader mirrorShader;

    Vec3 eyePosition {0.f, 0.f, orbitRadius};
    Vec3 upDirection {0.f, 1.f, 0.f};
    float elapsed = 0.f;
};

// The six face colours named on top of the GPU output, through the platform's
// own 2D pipeline, so what is on screen can be matched to what was uploaded
// without counting round the axes by hand.
struct LegendView final : Graphics::View
{
    void paint(Graphics::Context& g) override
    {
        constexpr auto left = 18.f;
        constexpr auto facesLeft = 118.f;
        constexpr auto faceSpacing = 42.f;

        auto bounds = getLocalBounds();
        auto baseline = bounds.h - 18.f;

        g.setColor(Graphics::Color::white());
        g.drawText("sky: the view direction", {left, 26.f}, font);
        g.drawText(
            "ball: the eye vector reflected about the normal", {left, 44.f}, font);

        g.drawText("cube faces", {left, baseline}, font);

        for (auto i = 0; i < 6; ++i)
        {
            const auto& face = skyFaces[i];

            g.setColor(Graphics::Color {face.color.x, face.color.y, face.color.z});
            g.drawText(std::string {face.name},
                       {facesLeft + (float) i * faceSpacing, baseline},
                       font);
        }
    }

    Graphics::Font font {Graphics::FontOptions().withName("Menlo").withSize(13.f)};
};

struct RootView final : Graphics::View
{
    void resized() override
    {
        for (auto* child: getSubviews())
            child->setBounds(getLocalBounds());
    }
};

struct CubeMapApp
{
    CubeMapApp()
    {
        root.addSubview(scene);
        root.addSubview(legend);
        window.setContentView(root);
    }

    RootView root;
    CubeMapView scene;
    LegendView legend;
    Graphics::Window window {windowOptions()};
};

namespace
{
// Which face a pixel came from, by hue. Every texel of a face is that face's
// colour times a brightness, and scaling a colour does not turn it in RGB, so
// the nearest of the six *in direction* names the face - through a mip level, a
// bilinear blend and the sphere's reflection alike, none of which can mix in a
// seventh hue. Returns -1 for a pixel too dark to point anywhere.
int faceOf(const Graphics::Color& color)
{
    auto length =
        std::sqrt(color.r * color.r + color.g * color.g + color.b * color.b);

    if (length < 0.02f)
        return -1;

    auto best = -1;
    auto bestDot = 0.f;

    for (auto i = 0; i < 6; ++i)
    {
        auto face = normalize(skyFaces[i].color);
        auto similarity =
            (color.r * face.x + color.g * face.y + color.b * face.z) / length;

        if (similarity > bestDot)
        {
            bestDot = similarity;
            best = i;
        }
    }

    return best;
}

float luminance(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

char faceLetter(const Graphics::Color& color)
{
    auto face = faceOf(color);
    return face < 0 ? '.' : skyFaces[face].letter;
}

// The brightness the face's colour was scaled by, recovered by dividing that
// colour back out - the pattern with the hue taken off it. A blue face is a
// third as bright as a red one at the same point of the gradient, so a raw
// luminance would say more about which face is on screen than about which way
// round it is, and it is the second that both the grid below and the
// orientation checks are asking about.
float brightnessOf(const Graphics::Color& color)
{
    auto face = faceOf(color);

    if (face < 0)
        return 0.f;

    const auto& hue = skyFaces[face].color;
    auto scale =
        luminance(color.r, color.g, color.b) / luminance(hue.x, hue.y, hue.z);

    return std::clamp(scale, 0.f, 1.f);
}

// The mean of a band along one edge of the frame. Only ever asked about the
// outermost few rows or columns, which is where the sky is: the ball covers the
// middle 45% of the height and comes nowhere near.
float bandMean(const Graphics::Image& image, int x0, int y0, int x1, int y1)
{
    auto total = 0.f;

    for (auto y = y0; y < y1; ++y)
        for (auto x = x0; x < x1; ++x)
            total += brightnessOf(image.at(x, y));

    return total / (float) ((x1 - x0) * (y1 - y0));
}

// One oblique view, printed twice: which face each pixel was sampled from, and
// how bright it came back. The first says where the six went, the second shows
// the gradient and the grid inside them - so between them the whole convention
// is on the page.
void printFaceMap(CubeMapView& view)
{
    constexpr auto width = 76;
    constexpr auto height = 30;

    // From the +X +Y +Z octant, so the three faces the camera is looking at are
    // -X, -Y and -Z and the ball, reflecting back past the camera, shows +X, +Y
    // and +Z. Lower case ahead, upper case in the ball.
    //
    // Closer in than the orbit runs, because the ball is what is worth looking
    // at here and 76 columns is not many to give it.
    auto eye = normalize({0.62f, 0.45f, 0.65f}) * (orbitRadius * 0.7f);

    view.lookFrom(eye, {0.f, 1.f, 0.f});
    view.setBounds({0.f, 0.f, (float) width, (float) height});

    auto image = view.renderToImage(1.f);

    if (!image.isValid())
        return;

    std::printf("\nfaces, from above and to the right of the ball\n\n");

    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
            std::printf("%c", faceLetter(image.at(x, y)));

        std::printf("\n");
    }

    std::printf("\n ");

    for (const auto& face: skyFaces)
        std::printf("  %c %s", face.letter, face.name);

    std::printf("   . nothing\n\nthe pattern in the same frame, hue divided out"
                " - bright toward u = 1 and v = 0\n\n");

    const char* ramp = " .:-=+*#%@";

    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            auto step = (int) (brightnessOf(image.at(x, y)) * 9.999f);
            std::printf("%c", ramp[std::clamp(step, 0, 9)]);
        }

        std::printf("\n");
    }
}

// The camera on each axis in turn, looking at the ball, and four questions per
// pose that between them pin both halves of the convention.
//
// **Which face.** The sky ahead of a camera on +X is the -X face, and the ball
// shows +X: the point of it nearest the camera has a normal pointing straight
// back at the eye, so the eye vector reflects into itself reversed and reads
// the face behind the camera. Six poses, and the six faces answer once each,
// twice over.
//
// **Which way up.** The up vectors below are chosen so that all six poses agree
// with each other: v runs down the frame, and u runs *right to left*. The
// second is the one that surprises people, and it is not a bug in anything - a
// cube map is looked at from the inside, so every face reads mirrored compared
// with the same image laid out on a table. The gradient is what says so: it
// rises toward u = 1 and toward v = 0, so the top of the frame must come back
// brighter than the bottom, and the left brighter than the right.
int printAxisChecks(CubeMapView& view)
{
    constexpr auto size = 32;
    constexpr auto band = 3;

    // Narrow enough that the whole frame lands on the face ahead - the corners
    // of a 50 degree square lens are 33 degrees off axis, and a face reaches 45.
    constexpr Vec3 eyes[6] = {
        {orbitRadius, 0.f, 0.f},
        {-orbitRadius, 0.f, 0.f},
        {0.f, orbitRadius, 0.f},
        {0.f, -orbitRadius, 0.f},
        {0.f, 0.f, orbitRadius},
        {0.f, 0.f, -orbitRadius},
    };

    // +Y is the only axis where +Y cannot be up. The two that look along it
    // take +Z and -Z instead, which is what makes the frame's orientation
    // answer the same way as the other four.
    constexpr Vec3 ups[6] = {
        {0.f, 1.f, 0.f},
        {0.f, 1.f, 0.f},
        {0.f, 0.f, 1.f},
        {0.f, 0.f, -1.f},
        {0.f, 1.f, 0.f},
        {0.f, 1.f, 0.f},
    };

    std::printf(
        "\ncamera  sky ahead         ball centre       v down  u leftward\n");

    auto failures = 0;

    for (auto axis = 0; axis < 6; ++axis)
    {
        view.lookFrom(eyes[axis], ups[axis]);
        view.setBounds({0.f, 0.f, (float) size, (float) size});

        auto image = view.renderToImage(1.f);

        if (!image.isValid())
            return 1;

        // The opposite face is the one ahead, and this axis's own face is the
        // one the ball sends back.
        auto ahead = axis ^ 1;

        auto skyFace = faceOf(image.at(size / 8, size / 8));
        auto ballFace = faceOf(image.at(size / 2, size / 2));

        auto top = bandMean(image, 0, 0, size, band);
        auto bottom = bandMean(image, 0, size - band, size, size);
        auto left = bandMean(image, 0, 0, band, size);
        auto right = bandMean(image, size - band, 0, size, size);

        auto skyOk = skyFace == ahead;
        auto ballOk = ballFace == axis;
        auto verticalOk = top > bottom;
        auto horizontalOk = left > right;

        failures += (int) (!skyOk) + (int) (!ballOk) + (int) (!verticalOk)
                    + (int) (!horizontalOk);

        auto nameOf = [](int face) { return face < 0 ? "--" : skyFaces[face].name; };

        std::printf("%-7s %-2s want %-2s %-5s %-2s want %-2s %-5s %-7s %s\n",
                    skyFaces[axis].name,
                    nameOf(skyFace),
                    skyFaces[ahead].name,
                    skyOk ? "ok" : "FAIL",
                    nameOf(ballFace),
                    skyFaces[axis].name,
                    ballOk ? "ok" : "FAIL",
                    verticalOk ? "ok" : "FAIL",
                    horizontalOk ? "ok" : "FAIL");
    }

    std::printf("\n%d of 24 checks failed\n", failures);

    return failures == 0 ? 0 : 1;
}

// What stands in for looking at the window. Everything on screen here is one
// cube lookup, so a frame read back is a statement about the cube and nothing
// else - which face a direction landed on, and which way round the face was.
int runCheck()
{
    auto view = CubeMapView {1};

    view.setBounds({0.f, 0.f, 32.f, 32.f});

    if (!view.renderToImage(1.f).isValid())
    {
        std::printf("no GPU device\n");
        return 1;
    }

    printFaceMap(view);

    return printAxisChecks(view);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--check") == 0)
        return runCheck();

    return eacp::Apps::run<CubeMapApp>();
}
