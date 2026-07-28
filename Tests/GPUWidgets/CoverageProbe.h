#pragma once

#include <eacp/GPUWidgets/GPUWidgets.h>

// The coverage a rasterized path actually got, texel for texel, with nothing in
// between.
//
// Drawing the mask and reading the pixels back instead would put a whole
// compositor in the way - an alpha blend, a premultiply, and the display's
// transfer function, which alone bends a coverage of 0.61 into 0.80. So this
// takes a second compute pass to copy the texture into a buffer instead: the
// fetch reads the texel the kernel wrote, commit() blocks until it exists, and
// no window is involved, which is what lets it run wherever the suite does.
namespace eacp::GPUWidgets::probe
{
struct MaskReadKernel final : GPU::ComputeProgram
{
    MaskReadKernel() { compile(); }

    void define() override
    {
        auto texel = threadPosition();
        auto value = fetch(mask, float2(toFloat(texel.x), toFloat(texel.y)));

        write(coverage, texel.y * maskWidth + texel.x, value.x());
    }

    GPU::Uniform<GPU::Texture2D> mask;
    GPU::Uniform<GPU::OutputBuffer> coverage;
    GPU::Uniform<GPU::UInt> maskWidth;

    EACP_SHADER(mask, coverage, maskWidth)
};

// One per process, like the coverage kernel itself: building a library and a
// pipeline per test would be most of the suite's time.
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

// Row-major, one float per coverage pixel. Empty when there is no device, or
// nothing to rasterize - so a caller checks that before reading anything into
// the result.
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

        auto pass = commands.beginCompute();
        pass.dispatch(reader, width, height);
    }

    commands.commit();

    coverage.resize(width * height);
    readback.read(coverage.data(), bytes);
    return coverage;
}
} // namespace eacp::GPUWidgets::probe
