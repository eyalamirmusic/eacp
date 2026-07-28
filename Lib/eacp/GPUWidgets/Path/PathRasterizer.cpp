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
} // namespace

PathRasterizer::PathRasterizer()
{
    kernel.prepare();
}

void PathRasterizer::setScale(float pixelsPerUnit)
{
    scale = pixelsPerUnit;
}

bool PathRasterizer::isEmpty() const
{
    return segments.empty() || coverageWidth <= 0 || coverageHeight <= 0;
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

    kernel.segmentCount = segments.size() / 4;
    kernel.evenOdd = rule == FillRule::EvenOdd ? 1 : 0;

    resizeCoverage(coverageWidth, coverageHeight);
    uploadSegments();
}

void PathRasterizer::resizeCoverage(int width, int height)
{
    if (coverageTexture.has_value() && coverageTexture->width() == width
        && coverageTexture->height() == height)
        return;

    coverageTexture.emplace(
        GPU::Device::shared(), describeCoverage(width, height), nullptr);

    kernel.coverage = *coverageTexture;
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

    kernel.segments = *segmentBuffer;
}

void PathRasterizer::dispatch(GPU::ComputePass& pass)
{
    if (isEmpty())
        return;

    pass.dispatch(kernel, coverageWidth, coverageHeight);
}
} // namespace eacp::GPUWidgets
