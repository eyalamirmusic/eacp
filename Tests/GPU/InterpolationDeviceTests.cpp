#include "Common.h"

#include <cmath>
#include <functional>
#include <string>

// Interpolation qualifiers on the device, each told apart from the others by
// what the rasterizer hands the fragment stage - the spellings are pinned by
// InterpolationCodegenTests.
//
// Perspective against linear: a quad whose w runs from 1 on the left to 4 on
// the right, with a value from 0 to 1 across it. Linear gives the screen
// fraction t at each pixel centre, perspective (t/4) / ((1 - t) + t/4), so the
// two differ by up to 0.3 in the middle. Flat: four triangles with a distinct
// value at each vertex, so any pixel that is not exactly the first vertex's
// value was interpolated or took another vertex; then a triangle strip, whose
// second triangle's first vertex is the strip's second. Centroid: a triangle
// on a four-sample target whose hypotenuse misses every pixel centre, carrying
// its barycentric coordinates - at the centre of a pixel it only partly covers
// one goes negative, at the centroid none does. Sample: the varying is the
// pixel coordinate, and the distance of its fraction from one half is zero at
// every centre and not at any sample of the standard patterns.
//
// Self-skips without a device; the multisampled cases without four samples,
// and the per-sample one without Device::supportsSampleRateShading.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace InterpolationDeviceTests
{
namespace
{
constexpr auto targetSize = 8;
constexpr auto pixelCount = targetSize * targetSize;
constexpr auto msaaSize = 16;
constexpr auto msaaPixelCount = msaaSize * msaaSize;
constexpr auto samples = 4;

struct DepthVertex
{
    float position[2];
    float w;
    float value;
};

struct ValueVertex
{
    float position[2];
    float value;
};

struct CornerVertex
{
    float position[2];
    float corner[3];
};

constexpr DepthVertex receding[] = {
    {{-1.f, -1.f}, 1.f, 0.f},
    {{1.f, -1.f}, 4.f, 1.f},
    {{-1.f, 1.f}, 1.f, 0.f},
    {{1.f, -1.f}, 4.f, 1.f},
    {{1.f, 1.f}, 4.f, 1.f},
    {{-1.f, 1.f}, 1.f, 0.f},
};

// Triangle t's vertex k holds 10t + k.
constexpr ValueVertex fourTriangles[] = {
    {{-1.f, -1.f}, 0.f},
    {{0.f, -1.f}, 1.f},
    {{-1.f, 1.f}, 2.f},
    {{0.f, -1.f}, 10.f},
    {{0.f, 1.f}, 11.f},
    {{-1.f, 1.f}, 12.f},
    {{0.f, -1.f}, 20.f},
    {{1.f, -1.f}, 21.f},
    {{0.f, 1.f}, 22.f},
    {{1.f, -1.f}, 30.f},
    {{1.f, 1.f}, 31.f},
    {{0.f, 1.f}, 32.f},
};

constexpr ValueVertex strip[] = {
    {{-1.f, -1.f}, 1.f},
    {{1.f, -1.f}, 2.f},
    {{-1.f, 1.f}, 3.f},
    {{1.f, 1.f}, 4.f},
};

// The hypotenuse runs along x + y = 14.7 pixels of the 16, so the centres of
// the pixels it cuts (x + y = 15) lie 0.3 outside it, where the A corner's
// weight is -0.02, while the samples a standard pattern puts half a pixel back
// along the diagonal lie inside.
constexpr CornerVertex corneredTriangle[] = {
    {{-1.f, -1.f}, {1.f, 0.f, 0.f}},
    {{0.8375f, -1.f}, {0.f, 1.f, 0.f}},
    {{-1.f, 0.8375f}, {0.f, 0.f, 1.f}},
};

constexpr ValueVertex fullQuad[] = {
    {{-1.f, -1.f}, 0.f},
    {{1.f, -1.f}, 0.f},
    {{-1.f, 1.f}, 0.f},
    {{1.f, -1.f}, 0.f},
    {{1.f, 1.f}, 0.f},
    {{-1.f, 1.f}, 0.f},
};

// The value unasked, asked perspective and asked linear.
struct RecedingShader final : ShaderProgram
{
    RecedingShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&DepthVertex::position);
        auto w = vertexInput(&DepthVertex::w);
        auto value = vertexInput(&DepthVertex::value);

        setPosition(float4(position * w, 0.f, w));
        setFragment(float4(varying(value),
                           varying(value, Interpolation::perspective()),
                           varying(value, Interpolation::linear()),
                           1.f));
    }

    EACP_SHADER()
};

// Flat first, flat either, and the vertex index unasked, which an integer
// varying takes flat.
struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&ValueVertex::position);
        auto value = vertexInput(&ValueVertex::value);

        setPosition(float4(position, 0.f, 1.f));

        auto either = Interpolation::flat(InterpolationSampling::Either);

        setFragment(float4(varying(value, Interpolation::flat()),
                           varying(value, either),
                           toFloat(varying(vertexId())),
                           1.f));
    }

    EACP_SHADER()
};

// One where a pixel centre fell outside the triangle, in red for the centre,
// green for the perspective centroid and blue for the linear one.
struct CentroidShader final : ShaderProgram
{
    CentroidShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&CornerVertex::position);
        auto corner = vertexInput(&CornerVertex::corner);

        setPosition(float4(position, 0.f, 1.f));

        auto centroid = InterpolationSampling::Centroid;
        auto atCentre = varying(corner);
        auto atPerspectiveCentroid =
            varying(corner, Interpolation::perspective(centroid));
        auto atLinearCentroid = varying(corner, Interpolation::linear(centroid));

        setFragment(float4(outside(atCentre),
                           outside(atPerspectiveCentroid),
                           outside(atLinearCentroid),
                           1.f));
    }

    static Float outside(const Float3& corner)
    {
        auto lowest = min(min(corner.x(), corner.y()), corner.z());
        return select(lowest < -0.01f, 1.f, 0.f);
    }

    EACP_SHADER()
};

// The same test with the corners sampled at each sample.
struct SampledCornerShader final : ShaderProgram
{
    SampledCornerShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&CornerVertex::position);
        auto corner = vertexInput(&CornerVertex::corner);

        setPosition(float4(position, 0.f, 1.f));

        auto atSample = varying(
            corner, Interpolation::perspective(InterpolationSampling::Sample));
        auto flagged = CentroidShader::outside(atSample);
        setFragment(float4(flagged, flagged, flagged, 1.f));
    }

    EACP_SHADER()
};

// Twice the squared distance of the pixel coordinate's fraction from the
// centre of its pixel, the coordinate interpolated linearly as asked.
struct PixelOffsetShader final : ShaderProgram
{
    explicit PixelOffsetShader(InterpolationSampling samplingToUse)
        : sampling(samplingToUse)
    {
        compile();
    }

    void define() override
    {
        auto position = vertexInput(&ValueVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto pixel = varying((position * 0.5f + 0.5f) * (float) msaaSize,
                             Interpolation::linear(sampling));
        auto offset = fract(pixel) - 0.5f;
        auto spread = dot(offset, offset) * 2.f;
        setFragment(float4(spread, spread, spread, 1.f));
    }

    InterpolationSampling sampling;

    EACP_SHADER()
};

struct FrameRunner final : GPUView
{
    FrameRunner() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        draw(frame);
        frame.beginPass({Graphics::Color::black()});
    }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

void drawAndRead(Texture& target,
                 void* pixels,
                 const std::function<void(RenderPass&)>& draw)
{
    auto view = FrameRunner {};

    auto drawFrame = [&](Frame& frame)
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = Graphics::Color::black();

            auto pass = frame.beginPass(target, descriptor);
            draw(pass);
        }

        frame.flush();
        target.read(pixels);
    };

    view.draw = drawFrame;
    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    check(view.renderToImage(1.f).isValid());
}

using FloatPixels = Array<float, pixelCount * 4>;
using BytePixels = Array<unsigned char, msaaPixelCount * 4>;

Texture makeFloatTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::RGBA32Float;
    descriptor.renderTarget = true;
    return Device::shared().makeTexture(descriptor);
}

Texture makeMultisampledTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = msaaSize;
    descriptor.height = msaaSize;
    descriptor.format = TextureFormat::RGBA8Unorm;
    descriptor.renderTarget = true;
    descriptor.sampleCount = samples;
    return Device::shared().makeTexture(descriptor);
}

RenderPipelineDescriptor
    describeFloatPipeline(PrimitiveTopology topology = PrimitiveTopology::Triangles)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA32Float);
    descriptor.topology = topology;
    return descriptor;
}

RenderPipelineDescriptor describeMultisampledPipeline()
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = samples;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::RGBA8Unorm);
    return descriptor;
}

template <typename Program>
FloatPixels drawFloat(Program& program)
{
    auto target = makeFloatTarget();
    auto pixels = FloatPixels {};

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    drawAndRead(target, pixels.data(), draw);
    return pixels;
}

template <typename Program>
BytePixels drawMultisampled(Program& program)
{
    program.prepare(describeMultisampledPipeline());

    auto target = makeMultisampledTarget();
    auto pixels = BytePixels {};

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    drawAndRead(target, pixels.data(), draw);
    return pixels;
}

bool canTest()
{
    return Device::shared().isValid();
}

bool canMultisample()
{
    return canTest() && Device::shared().supportsSampleCount(samples);
}

bool near(float value, float expected, float tolerance = 0.002f)
{
    return std::abs(value - expected) < tolerance;
}

float perspectiveAt(float t)
{
    return (t / 4.f) / ((1.f - t) + t / 4.f);
}

// The largest value a channel took anywhere.
int largest(const BytePixels& pixels, int channel)
{
    auto most = 0;

    for (auto i = 0; i < msaaPixelCount; ++i)
        if (pixels[i * 4 + channel] > most)
            most = pixels[i * 4 + channel];

    return most;
}

int smallest(const BytePixels& pixels, int channel)
{
    auto least = 255;

    for (auto i = 0; i < msaaPixelCount; ++i)
        if (pixels[i * 4 + channel] < least)
            least = pixels[i * 4 + channel];

    return least;
}

// Each pixel's red, which a flat varying must have taken whole from one vertex.
struct FlatValues
{
    explicit FlatValues(const FloatPixels& pixels)
    {
        for (auto i = 0; i < pixelCount; ++i)
        {
            const auto* pixel = pixels.data() + i * 4;
            const auto value = (int) std::lround(pixel[0]);

            allWhole = allWhole && near(pixel[0], (float) value, 1e-4f)
                       && pixel[1] == pixel[0];

            if (value >= 0 && value < (int) seen.size())
                seen[value] = true;
            else
                allWhole = false;

            vertexIds[i] = pixel[2];
        }
    }

    bool allWhole = true;
    Array<bool, 40> seen {};
    Array<float, pixelCount> vertexIds {};
};
} // namespace

auto tLinear = test("InterpolationDevice/linearSkipsThePerspectiveDivide") = []
{
    if (!canTest())
        return;

    auto program = RecedingShader {};
    program.setVertices(receding);
    program.prepare(describeFloatPipeline());

    auto pixels = drawFloat(program);

    for (auto y = 0; y < targetSize; ++y)
    {
        for (auto x = 0; x < targetSize; ++x)
        {
            const auto* pixel = pixels.data() + (y * targetSize + x) * 4;
            const auto t = ((float) x + 0.5f) / (float) targetSize;

            check(near(pixel[0], perspectiveAt(t)), std::to_string(pixel[0]));
            check(near(pixel[1], perspectiveAt(t)), std::to_string(pixel[1]));
            check(near(pixel[2], t), std::to_string(pixel[2]));
        }
    }
};

auto tFlat = test("InterpolationDevice/flatTakesEachTrianglesFirstVertex") = []
{
    if (!canTest())
        return;

    auto program = FlatShader {};
    program.setVertices(fourTriangles);
    program.prepare(describeFloatPipeline());

    const auto values = FlatValues {drawFloat(program)};

    check(values.allWhole);

    for (auto value = 0; value < (int) values.seen.size(); ++value)
        check(values.seen[value] == (value % 10 == 0), std::to_string(value));

    for (auto id: values.vertexIds)
        check(id == 0.f || id == 3.f || id == 6.f || id == 9.f, std::to_string(id));
};

// Triangle 0 of the strip is vertices 0, 1, 2 and triangle 1 is 1, 2, 3, wound
// the other way round, whose first vertex is still vertex 1.
auto tStrip = test("InterpolationDevice/aStripsTriangleTakesItsFirstVertex") = []
{
    if (!canTest())
        return;

    auto program = FlatShader {};
    program.setVertices(strip);
    program.prepare(describeFloatPipeline(PrimitiveTopology::TriangleStrip));

    const auto values = FlatValues {drawFloat(program)};

    check(values.allWhole);
    check(values.seen[1]);
    check(values.seen[2]);
    check(!values.seen[3]);
    check(!values.seen[4]);

    for (auto id: values.vertexIds)
        check(id == 0.f || id == 1.f, std::to_string(id));
};

auto tCentroid = test("InterpolationDevice/centroidStaysInsideThePrimitive") = []
{
    if (!canMultisample())
        return;

    auto program = CentroidShader {};
    program.setVertices(corneredTriangle);

    const auto pixels = drawMultisampled(program);

    check(largest(pixels, 0) >= 60, std::to_string(largest(pixels, 0)));
    check(largest(pixels, 1) <= 2, std::to_string(largest(pixels, 1)));
    check(largest(pixels, 2) <= 2, std::to_string(largest(pixels, 2)));
};

auto tSampleRateQuery =
    test("InterpolationDevice/theDeviceAnswersTheSampleRateQuery") = []
{
    const auto& device = Device::shared();

    if (!device.isValid())
    {
        check(!device.supportsSampleRateShading());
        return;
    }

    if (Platform::isApple() || Platform::isWindows())
        check(device.supportsSampleRateShading());

    // Mesa's lavapipe has it, so on CI the per-sample case runs.
    if (Platform::isLinux() && getEnvValue("EACP_VK_SOFTWARE") == "1")
        check(device.supportsSampleRateShading());

    auto centred = PixelOffsetShader {InterpolationSampling::Center};
    auto sampled = PixelOffsetShader {InterpolationSampling::Sample};

    check(centred.fitsSampleRateShading(device));
    check(sampled.fitsSampleRateShading(device)
          == device.supportsSampleRateShading());
};

auto tSample = test("InterpolationDevice/sampleInterpolatesAtEachSample") = []
{
    if (!canMultisample() || !Device::shared().supportsSampleRateShading())
        return;

    auto centred = PixelOffsetShader {InterpolationSampling::Center};
    centred.setVertices(fullQuad);
    const auto atCentre = drawMultisampled(centred);

    auto sampled = PixelOffsetShader {InterpolationSampling::Sample};
    sampled.setVertices(fullQuad);
    const auto atSamples = drawMultisampled(sampled);

    auto centroid = PixelOffsetShader {InterpolationSampling::Centroid};
    centroid.setVertices(fullQuad);
    const auto atCentroid = drawMultisampled(centroid);

    check(largest(atCentre, 0) <= 2, std::to_string(largest(atCentre, 0)));
    check(smallest(atSamples, 0) >= 20, std::to_string(smallest(atSamples, 0)));

    // A pixel the quad covers whole has its centroid at its centre; only the
    // two along the shared diagonal are covered in part by each triangle.
    auto awayFromTheDiagonal = 0;

    for (auto y = 0; y < msaaSize; ++y)
        for (auto x = 0; x < msaaSize; ++x)
            if (x != y && x != msaaSize - 1 - y
                && atCentroid[(y * msaaSize + x) * 4] > 2)
                ++awayFromTheDiagonal;

    check(awayFromTheDiagonal == 0, std::to_string(awayFromTheDiagonal));

    auto corners = SampledCornerShader {};
    corners.setVertices(corneredTriangle);
    check(largest(drawMultisampled(corners), 0) <= 2);
};
} // namespace InterpolationDeviceTests
