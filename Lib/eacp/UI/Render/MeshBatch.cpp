#include "MeshBatch.h"

#include "ClipShader.h"
#include "GradientShader.h"

#include <algorithm>

namespace eacp::UI
{
namespace
{
constexpr MeshCorner triangleCorners[] = {{0.f}, {1.f}, {2.f}};
} // namespace

struct MeshBatch::Program final : GPU::ShaderProgram
{
    Program()
    {
        // A mask is read at the size it was rasterized; a ramp is 256 texels
        // stretched over a shape.
        maskAtlas.sampling = {GPU::TextureFilter::Nearest,
                              GPU::TextureAddressMode::Clamp};

        gradientRamps.sampling = {GPU::TextureFilter::Linear,
                                  GPU::TextureAddressMode::Clamp};
        compile();
    }

    void define() override
    {
        auto corner = vertexInput(&MeshCorner::index);

        auto positionA = instanceInput(&MeshTriangle::positionA, 1);
        auto positionB = instanceInput(&MeshTriangle::positionB, 1);
        auto positionC = instanceInput(&MeshTriangle::positionC, 1);
        auto coverage = instanceInput(&MeshTriangle::coverage, 1);
        auto color = instanceInput(&MeshTriangle::color, 1);
        auto gradient = instanceInput(&MeshTriangle::gradient, 1);
        auto gradientRamp = instanceInput(&MeshTriangle::gradientRamp, 1);

        // Selected rather than indexed: the vertex stream is three constants.
        auto secondOrLater = step(0.5f, corner);
        auto third = step(1.5f, corner);

        auto position =
            mix(mix(positionA, positionB, secondOrLater), positionC, third);

        auto vertexCoverage =
            mix(mix(coverage.x(), coverage.y(), secondOrLater), coverage.z(), third);

        auto clipX = position.x() / screenSize.x() * 2.f - 1.f;
        auto clipY = 1.f - position.y() / screenSize.y() * 2.f;
        setPosition(float4(clipX, clipY, 0.f, 1.f));

        auto fragColor = varying(color);
        auto fragCoverage = varying(vertexCoverage);
        auto fragPosition = varying(position);
        auto fragGradient = varying(gradient);
        auto fragGradientRamp = varying(gradientRamp);

        // The gradient's kind rides in the coverage's fourth slot, which the
        // three corners left spare.
        auto fragKind = varying(coverage.w());

        auto fill = gradientFill(fragColor,
                                 fragPosition,
                                 fragGradient,
                                 fragGradientRamp,
                                 fragKind,
                                 gradientRamps);

        // Sampled exactly as the quad renderer samples it, so one clip cuts a
        // document drawn out of both.
        auto clipped = fragCoverage
                       * clipCoverage(fragPosition, clipRegion, clipMask, maskAtlas);

        setFragment(float4(fill.x(), fill.y(), fill.z(), fill.w() * clipped));
    }

    GPU::Uniform<GPU::Float2> screenSize;
    GPU::Uniform<GPU::Float4> clipRegion;
    GPU::Uniform<GPU::Float4> clipMask;
    GPU::Uniform<GPU::Texture2D> maskAtlas;
    GPU::Uniform<GPU::Texture2D> gradientRamps;

    EACP_SHADER(screenSize, clipRegion, clipMask, maskAtlas, gradientRamps)
};

MeshBatch::MeshBatch(const CoverageAtlas& atlasToUse,
                     GradientRamps& rampsToUse,
                     Point logicalSizeToUse,
                     int sampleCountToUse,
                     GPU::PixelFormat colorFormatToUse)
    : atlas(atlasToUse)
    , ramps(rampsToUse)
    , logicalSize(logicalSizeToUse)
    , sampleCount(sampleCountToUse)
    , colorFormat(colorFormatToUse)
{
    program.create();
    program->setVertices(triangleCorners);

    // Always blended, the feather ring being a coverage ramp, and in the same
    // mode as ShapeBatch so a shape drawn into a layer leaves the right alpha.
    program->prepare(sampleCount,
                     false,
                     GPU::PrimitiveTopology::Triangles,
                     GPU::BlendMode::AlphaBlendOntoTransparent,
                     colorFormat);
}

MeshBatch::~MeshBatch()
{
    detach();
}

void MeshBatch::begin(GPU::RenderPass& passToUse)
{
    detach();

    triangles.clear();
    pass = &passToUse;

    passToUse.addParticipant(*this);
}

void MeshBatch::end()
{
    flush();
    detach();
}

void MeshBatch::flushInto(GPU::RenderPass&)
{
    flush();

    // Not detach(): the pass is walking its participant list right now and drops
    // every one of them itself once the walk is over.
    pass = nullptr;
}

void MeshBatch::detach()
{
    if (pass == nullptr)
        return;

    pass->removeParticipant(*this);
    pass = nullptr;
}

void MeshBatch::setLogicalSize(Point size)
{
    logicalSize = {std::max(1.f, size.x), std::max(1.f, size.y)};
}

void MeshBatch::setClipMask(const ClipMask& mask)
{
    if (sameClipMask(clip, mask))
        return;

    flush();

    clip = mask;
}

void MeshBatch::flush()
{
    if (triangles.empty() || pass == nullptr)
        return;

    ramps.commit();

    program->screenSize = Array {logicalSize.x, logicalSize.y};

    packClipMask(clip,
                 atlas.getOpaqueUV(),
                 program->clipRegion.value,
                 program->clipMask.value);

    program->maskAtlas = atlas.getTexture();
    program->gradientRamps = ramps.getTexture();
    program->setInstances(1, triangles.data(), triangles.size());

    pass->drawInstanced(*program, triangles.size());

    triangles.clear();
}

void MeshBatch::addMesh(const Vector<GPUWidgets::MeshVertex>& mesh,
                        Point offset,
                        const Color& color,
                        const GradientFill& gradient)
{
    if (color.a <= 0.f)
        return;

    // Resolved once and copied into every triangle of the shape.
    auto packedGradient = MeshTriangle {};
    auto kind =
        packGradient(gradient, packedGradient.gradient, packedGradient.gradientRamp);

    auto count = mesh.size() / 3;
    triangles.reserve(triangles.size() + count);

    for (auto index = 0; index < count; ++index)
    {
        const auto& a = mesh[index * 3];
        const auto& b = mesh[index * 3 + 1];
        const auto& c = mesh[index * 3 + 2];

        auto triangle = MeshTriangle {};

        triangle.positionA[0] = a.position.x + offset.x;
        triangle.positionA[1] = a.position.y + offset.y;
        triangle.positionB[0] = b.position.x + offset.x;
        triangle.positionB[1] = b.position.y + offset.y;
        triangle.positionC[0] = c.position.x + offset.x;
        triangle.positionC[1] = c.position.y + offset.y;

        triangle.coverage[0] = a.coverage;
        triangle.coverage[1] = b.coverage;
        triangle.coverage[2] = c.coverage;
        triangle.coverage[3] = kind;

        triangle.color[0] = color.r;
        triangle.color[1] = color.g;
        triangle.color[2] = color.b;
        triangle.color[3] = color.a;

        std::copy(std::begin(packedGradient.gradient),
                  std::end(packedGradient.gradient),
                  std::begin(triangle.gradient));

        std::copy(std::begin(packedGradient.gradientRamp),
                  std::end(packedGradient.gradientRamp),
                  std::begin(triangle.gradientRamp));

        triangles.add(triangle);
    }
}
} // namespace eacp::UI
