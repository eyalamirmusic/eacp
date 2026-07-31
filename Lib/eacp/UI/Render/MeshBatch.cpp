#include "MeshBatch.h"

#include <algorithm>

namespace eacp::UI
{
namespace
{
constexpr MeshCorner triangleCorners[] = {{0.f}, {1.f}, {2.f}};
} // namespace

struct MeshBatch::Program final : GPU::ShaderProgram
{
    Program() { compile(); }

    void define() override
    {
        auto corner = vertexInput(&MeshCorner::index);

        auto positionA = instanceInput(&MeshTriangle::positionA, 1);
        auto positionB = instanceInput(&MeshTriangle::positionB, 1);
        auto positionC = instanceInput(&MeshTriangle::positionC, 1);
        auto coverage = instanceInput(&MeshTriangle::coverage, 1);
        auto color = instanceInput(&MeshTriangle::color, 1);

        // Which of the three this vertex is, selected rather than indexed: the
        // vertex stream is three constants and everything that varies is
        // per-instance, which is what lets a document's whole geometry go out as
        // one draw.
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

        setFragment(float4(fragColor.x(),
                           fragColor.y(),
                           fragColor.z(),
                           fragColor.w() * fragCoverage));
    }

    GPU::Uniform<GPU::Float2> screenSize;

    EACP_SHADER(screenSize)
};

MeshBatch::MeshBatch(Point logicalSizeToUse,
                     int sampleCountToUse,
                     GPU::PixelFormat colorFormatToUse)
    : logicalSize(logicalSizeToUse)
    , sampleCount(sampleCountToUse)
    , colorFormat(colorFormatToUse)
{
    program.create();
    program->setVertices(triangleCorners);

    // Always blended, for the same reason ShapeBatch is: the feather ring is a
    // coverage ramp, and without blending the antialiasing would punch holes in
    // whatever is behind it.
    program->prepare(sampleCount,
                     false,
                     GPU::PrimitiveTopology::Triangles,
                     GPU::BlendMode::AlphaBlend,
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

void MeshBatch::flush()
{
    if (triangles.empty() || pass == nullptr)
        return;

    program->screenSize = Array {logicalSize.x, logicalSize.y};
    program->setInstances(1, triangles.data(), triangles.size());

    pass->drawInstanced(*program, triangles.size());

    triangles.clear();
}

void MeshBatch::addMesh(const Vector<GPUWidgets::MeshVertex>& mesh,
                        Point offset,
                        const Color& color)
{
    if (color.a <= 0.f)
        return;

    // A flat triangle list, so three vertices are one triangle and a remainder
    // is geometry nobody built.
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

        triangle.color[0] = color.r;
        triangle.color[1] = color.g;
        triangle.color[2] = color.b;
        triangle.color[3] = color.a;

        triangles.add(triangle);
    }
}
} // namespace eacp::UI
