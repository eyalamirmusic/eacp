#include "Common.h"

#include <eacp/GPU/Codegen/PackedScalars.h>

#include <cmath>
#include <cstring>
#include <functional>

// Every uncompressed float and normalised format as a render target, drawn
// into by a pipeline built for pixelFormatFor(that format): the one- and
// two-channel formats a G-buffer, an occlusion buffer or a motion buffer is
// kept in, at their own size rather than widened. The shaders are
// hand-written.
//
// Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace FormatTargetTests
{
namespace
{
constexpr auto targetSize = 4;

struct PositionVertex
{
    float position[2];
};

constexpr PositionVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
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

void runFrame(std::function<void(Frame&)> draw)
{
    auto view = FrameRunner {};
    view.draw = std::move(draw);
    view.setBounds({0.f, 0.f, 4.f, 4.f});
    view.renderToImage(1.f);
}

struct TargetFormat
{
    TextureFormat format;
    int channels;
};

constexpr TargetFormat targetFormats[] = {
    {TextureFormat::R8Unorm, 1},
    {TextureFormat::RG8Unorm, 2},
    {TextureFormat::R16Float, 1},
    {TextureFormat::RG16Float, 2},
    {TextureFormat::RG32Float, 2},
    {TextureFormat::R16Unorm, 1},
};

float channelOf(TextureFormat format, const std::uint8_t* texel, int channel)
{
    auto bits = std::uint16_t {};
    auto value = 0.f;

    switch (format)
    {
        case TextureFormat::R8Unorm:
        case TextureFormat::RG8Unorm:
            return (float) texel[channel] / 255.f;

        case TextureFormat::R16Float:
        case TextureFormat::RG16Float:
            std::memcpy(&bits, texel + channel * 2, sizeof(bits));
            return halfToFloat(bits);

        case TextureFormat::R16Unorm:
            std::memcpy(&bits, texel + channel * 2, sizeof(bits));
            return (float) bits / 65535.f;

        default:
            std::memcpy(&value, texel + channel * 4, sizeof(value));
            return value;
    }
}
} // namespace

auto tFormatTargetDraws = test("FormatTarget/aPassDrawsIntoEachFormat") = []
{
    if (!Device::shared().isValid())
        return;

    // (0.25, 0.5, 0.75, 1): exact in a half and within a step of eight bits.
    auto source = nativeShaderSource("FormatTarget/flat");
    auto library = Device::shared().makeShaderLibrary(
        source.withVertex("vertexMain").withFragment("fragmentMain"));
    auto quad = Device::shared().makeBuffer(fullQuad, sizeof(fullQuad));

    for (const auto& target: targetFormats)
    {
        auto descriptor = TextureDescriptor {};
        descriptor.width = targetSize;
        descriptor.height = targetSize;
        descriptor.format = target.format;
        descriptor.renderTarget = true;

        auto texture = Device::shared().makeTexture(descriptor);

        auto pipelineDescriptor = RenderPipelineDescriptor {};
        pipelineDescriptor.library = &library;
        pipelineDescriptor.colorFormat = pixelFormatFor(target.format);
        pipelineDescriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
        pipelineDescriptor.vertexLayout.stride = sizeof(PositionVertex);

        auto pipeline = Device::shared().makeRenderPipeline(pipelineDescriptor);

        check(texture.isValid() && pipeline.isValid());

        runFrame(
            [&](Frame& frame)
            {
                auto pass = frame.beginPass(texture, {Graphics::Color::black()});
                pass.setPipeline(pipeline);
                pass.setVertexBuffer(quad);
                pass.draw(6);
            });

        const auto stride = bytesPerPixel(target.format);
        auto bytes = Vector<std::uint8_t> {};
        bytes.resize((std::size_t) (targetSize * targetSize * stride));
        texture.read(bytes.data());

        const float expected[] = {0.25f, 0.5f};

        for (auto texel = 0; texel < targetSize * targetSize; ++texel)
            for (auto channel = 0; channel < target.channels; ++channel)
                check(std::fabs(channelOf(target.format,
                                          bytes.data() + texel * stride,
                                          channel)
                                - expected[channel])
                      <= 2.5e-3f);
    }
};
} // namespace FormatTargetTests
