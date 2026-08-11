#pragma once

#include <eacp/GPUWidgets/GPUWidgets.h>

// Reads mask coverage through a compute pass rather than by drawing: a
// compositor in the way would premultiply and gamma-encode the value, bending a
// coverage of 0.61 into 0.80.
namespace eacp::GPUWidgets::probe
{
struct MaskReadKernel final : GPU::ComputeProgram
{
    MaskReadKernel() { compile(); }

    void define() override
    {
        auto texel = threadPosition();
        auto value = fetch(
            mask, float2(toFloat(texel.x + originX), toFloat(texel.y + originY)));

        write(coverage, texel.y * maskWidth + texel.x, value.x());
    }

    GPU::Uniform<GPU::Texture2D> mask;
    GPU::Uniform<GPU::OutputBuffer> coverage;
    GPU::Uniform<GPU::UInt> maskWidth;

    GPU::Uniform<GPU::UInt> originX;
    GPU::Uniform<GPU::UInt> originY;

    EACP_SHADER(mask, coverage, maskWidth, originX, originY)
};

// One per process: building a library and a pipeline per test would be most of
// the suite's time.
inline MaskReadKernel& maskReader()
{
    struct Prepared
    {
        Prepared() { kernel.prepare(); }

        MaskReadKernel kernel;
    };

    static auto prepared = Prepared {};
    return prepared.kernel;
}

// One rect of a mask texture, row-major, one float per texel.
inline Vector<float>
    readRegion(const GPU::Texture& texture, int x, int y, int width, int height)
{
    auto values = Vector<float> {};

    if (!GPU::Device::shared().isValid() || width <= 0 || height <= 0)
        return values;

    auto bytes = sizeof(float) * (std::size_t) (width * height);
    auto readback = GPU::Buffer {
        GPU::Device::shared(), nullptr, bytes, GPU::BufferUsage::Storage};

    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto& reader = maskReader();
        reader.mask = texture;
        reader.coverage = readback;
        reader.maskWidth = (std::uint32_t) width;
        reader.originX = (std::uint32_t) x;
        reader.originY = (std::uint32_t) y;

        auto pass = commands.beginCompute();
        pass.dispatch(reader, width, height);
    }

    commands.commit();

    values.resize(width * height);
    readback.read(values.data(), bytes);
    return values;
}

// Row-major, one float per coverage pixel. Empty when there is no device or
// nothing to rasterize, so callers check that before reading it.
inline Vector<float> rasterize(PathRasterizer& rasterizer,
                               const Path& path,
                               float scale,
                               FillRule rule = FillRule::NonZero)
{
    auto coverage = Vector<float> {};

    if (!GPU::Device::shared().isValid())
        return coverage;

    rasterizer.setScale(scale);
    rasterizer.setPath(path, rule);

    if (rasterizer.isEmpty())
        return coverage;

    auto width = rasterizer.getCoverageWidth();
    auto height = rasterizer.getCoverageHeight();
    auto bytes = sizeof(float) * (std::size_t) (width * height);

    auto readback = GPU::Buffer {
        GPU::Device::shared(), nullptr, bytes, GPU::BufferUsage::Storage};

    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        rasterizer.dispatch(pass);
    }

    {
        auto& reader = maskReader();
        reader.mask = rasterizer.getCoverage();
        reader.coverage = readback;
        reader.maskWidth = (std::uint32_t) width;
        reader.originX = 0u;
        reader.originY = 0u;

        auto pass = commands.beginCompute();
        pass.dispatch(reader, width, height);
    }

    commands.commit();

    coverage.resize(width * height);
    readback.read(coverage.data(), bytes);
    return coverage;
}
} // namespace eacp::GPUWidgets::probe
