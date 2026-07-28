#include "PathRasterizer.h"

#include <cmath>

namespace eacp::GPUWidgets
{
namespace
{
GPU::TextureDescriptor describeCoverage(int width, int height)
{
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;
    descriptor.computeWrite = true;
    return descriptor;
}

// One kernel for every rasterizer there will ever be. It is the same program in
// all of them, and building one costs a shader library and a compute pipeline -
// which an interface with a rasterizer per widget must not pay per widget.
//
// Shared state is safe here because a dispatch sets every uniform it reads
// immediately before issuing it, and command encoding is single-threaded. Built
// on first use rather than at load, since it needs the Device - which also puts
// its destruction before the Device's own, statics tearing down in reverse.
CoverageKernel& sharedKernel()
{
    struct Prepared
    {
        Prepared() { kernel.prepare(); }

        CoverageKernel kernel;
    };

    static auto prepared = Prepared {};
    return prepared.kernel;
}
} // namespace

void PathRasterizer::setScale(float pixelsPerUnit)
{
    scale = pixelsPerUnit;
}

bool PathRasterizer::isEmpty() const
{
    return segments.empty() || coverageWidth <= 0 || coverageHeight <= 0;
}

void PathRasterizer::setTarget(const GPU::Texture& texture,
                               int originXToUse,
                               int originYToUse)
{
    target = &texture;
    originX = originXToUse;
    originY = originYToUse;

    // Its own texture is now dead weight, and holding it would keep a whole
    // path's worth of pixels alive for as long as the shape exists.
    coverageTexture.reset();
}

void PathRasterizer::clearTarget()
{
    target = nullptr;
    originX = 0;
    originY = 0;
}

void PathRasterizer::setPath(const Path& path, FillRule rule)
{
    segments.clear();
    covered = {};
    coverageWidth = 0;
    coverageHeight = 0;

    if (path.isEmpty() || scale <= 0.f)
        return;

    // A pixel of margin on every side: an edge landing exactly on the bounding
    // box still spills coverage into the pixel beyond it.
    auto bounds = path.getBounds();
    auto left = std::floor(bounds.x * scale) - 1.f;
    auto top = std::floor(bounds.y * scale) - 1.f;
    auto right = std::ceil((bounds.x + bounds.w) * scale) + 1.f;
    auto bottom = std::ceil((bounds.y + bounds.h) * scale) + 1.f;

    coverageWidth = (int) (right - left);
    coverageHeight = (int) (bottom - top);
    covered = {
        left / scale, top / scale, (right - left) / scale, (bottom - top) / scale};

    for (const auto& sub: path.getSubPaths())
    {
        const auto& points = sub.points;

        if (points.size() < 2)
            continue;

        // Wrapping past the last point emits the closing segment explicitly, so
        // every sub-path reaches the kernel as a closed loop - which a fill
        // treats it as whether or not close() was called.
        for (auto i = 0; i < points.size(); ++i)
        {
            const auto& from = points[i];
            const auto& to = points[(i + 1) % points.size()];

            auto fromY = from.y * scale - top;
            auto toY = to.y * scale - top;

            // A horizontal segment contributes nothing to any pixel, so it is
            // dropped once here rather than guarded against once per pixel.
            if (fromY == toY)
                continue;

            segments.add(from.x * scale - left);
            segments.add(fromY);
            segments.add(to.x * scale - left);
            segments.add(toY);
        }
    }

    if (isEmpty())
        return;

    segmentCount = segments.size() / 4;
    evenOdd = rule == FillRule::EvenOdd ? 1 : 0;

    uploadSegments();
}

void PathRasterizer::ensureOwnTexture()
{
    if (coverageTexture.has_value() && coverageTexture->width() == coverageWidth
        && coverageTexture->height() == coverageHeight)
        return;

    coverageTexture.emplace(GPU::Device::shared(),
                            describeCoverage(coverageWidth, coverageHeight),
                            nullptr);
}

void PathRasterizer::uploadSegments()
{
    auto bytes = sizeof(float) * (std::size_t) segments.size();

    // Grown by reallocation and refilled in place otherwise: a path re-drawn at
    // the same complexity - a knob turning, a curve dragged - reuses the buffer.
    if (!segmentBuffer.has_value() || segmentBuffer->size() < bytes)
        segmentBuffer.emplace(GPU::Device::shared(),
                              segments.data(),
                              bytes,
                              GPU::BufferUsage::Storage);
    else
        segmentBuffer->update(segments.data(), bytes);
}

void PathRasterizer::dispatch(GPU::ComputePass& pass)
{
    if (isEmpty())
        return;

    if (target == nullptr)
        ensureOwnTexture();

    auto& kernel = sharedKernel();

    kernel.coverage = target != nullptr ? *target : *coverageTexture;
    kernel.segments = *segmentBuffer;
    kernel.segmentCount = segmentCount;
    kernel.evenOdd = evenOdd;
    kernel.originX = (std::uint32_t) originX;
    kernel.originY = (std::uint32_t) originY;

    pass.dispatch(kernel, coverageWidth, coverageHeight);
}
} // namespace eacp::GPUWidgets
