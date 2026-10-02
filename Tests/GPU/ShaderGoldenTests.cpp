#include "ShaderGolden.h"

#include <eacp/GPUWidgets/Path/BackdropKernels.h>
#include <eacp/GPUWidgets/Path/BinKernels.h>
#include <eacp/GPUWidgets/Path/CoverageKernel.h>
#include <eacp/GPUWidgets/Path/PrefixSum.h>
#include <eacp/GPUWidgets/View/CoverageShader.h>
#include <eacp/GPUWidgets/View/PathFillShader.h>
#include <eacp/GPUWidgets/View/VertexColorShader.h>
#include <eacp/ML/Kernels/Activation.h>
#include <eacp/ML/Kernels/Attention.h>
#include <eacp/ML/Kernels/BandedAttention.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Kernels/Norm.h>
#include <eacp/ML/Kernels/RoPE.h>
#include <eacp/ML/Kernels/SwiGLU.h>
#include <eacp/ML/Kernels/TensorOps.h>
#include <eacp/Sprites/SpriteRenderer.h>
#include <eacp/Text/GlyphRenderer.h>
#include <eacp/UI/Render/ImageBatch.h>
#include <eacp/UI/Render/LayerRenderer.h>
#include <eacp/UI/Render/MeshBatch.h>
#include <eacp/UI/Render/ShapeBatch.h>

// Every shader the library ships. A kernel added to ML, GPUWidgets, Sprites,
// UI/Render or Text gets a line here; one removed loses its line, and the
// noOrphans case fails until its files go too.

using namespace eacp;
using namespace eacp::ShaderGolden;

namespace
{
using GPU::TextureAddressMode;
using GPU::TextureFilter;

void addMLKernels(std::vector<Entry>& entries)
{
    using namespace ML;

    entries.insert(
        entries.end(),
        {
            {"ML/ActivationKernel.SiLU",
             program<ActivationKernel>(ActivationKind::SiLU)},
            {"ML/ActivationKernel.GeluTanh",
             program<ActivationKernel>(ActivationKind::GeluTanh)},
            {"ML/ActivationKernel.GeluExact",
             program<ActivationKernel>(ActivationKind::GeluExact)},
            {"ML/ActivationKernel.Sigmoid",
             program<ActivationKernel>(ActivationKind::Sigmoid)},
            {"ML/AttentionScoresKernel.Additive",
             program<AttentionScoresKernel>(AttentionMask::Additive)},
            {"ML/AttentionScoresKernel.None",
             program<AttentionScoresKernel>(AttentionMask::None)},
            {"ML/AttentionRowStatsKernel", program<AttentionRowStatsKernel>()},
            {"ML/AttentionWeightedSumKernel", program<AttentionWeightedSumKernel>()},
            {"ML/BandedAttentionScoresKernel",
             program<BandedAttentionScoresKernel>()},
            {"ML/BandedAttentionRowStatsKernel",
             program<BandedAttentionRowStatsKernel>()},
            {"ML/BandedAttentionWeightedSumKernel",
             program<BandedAttentionWeightedSumKernel>()},
            {"ML/LinearF32.FourWide.Rows64",
             program<LinearF32>(LinearLoads::FourWide, 64)},
            {"ML/LinearF32.FourWide.Rows32",
             program<LinearF32>(LinearLoads::FourWide, 32)},
            {"ML/LinearF32.Scalar.Rows64",
             program<LinearF32>(LinearLoads::Scalar, 64)},
            {"ML/LinearF32.Scalar.Rows32",
             program<LinearF32>(LinearLoads::Scalar, 32)},
            {"ML/LinearPackedHalf", program<LinearPackedHalf>()},
            {"ML/AddBiasRows", program<AddBiasRows>()},
            {"ML/RMSNormKernel", program<RMSNormKernel>()},
            {"ML/LayerNormKernel", program<LayerNormKernel>()},
            {"ML/LayerNormNoBiasKernel", program<LayerNormNoBiasKernel>()},
            {"ML/DynamicTanhKernel", program<DynamicTanhKernel>()},
            {"ML/RoPEKernel", program<RoPEKernel>()},
            {"ML/SwiGLUGateKernel", program<SwiGLUGateKernel>()},
            {"ML/ElementwiseKernel.Add",
             program<ElementwiseKernel>(ElementwiseOp::Add)},
            {"ML/ElementwiseKernel.Subtract",
             program<ElementwiseKernel>(ElementwiseOp::Subtract)},
            {"ML/ElementwiseKernel.Multiply",
             program<ElementwiseKernel>(ElementwiseOp::Multiply)},
            {"ML/ScaleAndAddKernel", program<ScaleAndAddKernel>()},
            {"ML/FillKernel", program<FillKernel>()},
            {"ML/CopyRowsKernel", program<CopyRowsKernel>()},
            {"ML/SliceColumnsKernel", program<SliceColumnsKernel>()},
        });
}

void addGPUWidgetsShaders(std::vector<Entry>& entries)
{
    using namespace GPUWidgets;

    entries.insert(
        entries.end(),
        {
            {"GPUWidgets/CoverageKernel", program<CoverageKernel>()},
            {"GPUWidgets/BinKernel", program<BinKernel>()},
            {"GPUWidgets/BackdropScanKernel", program<BackdropScanKernel>()},
            {"GPUWidgets/ClearKernel", program<ClearKernel>()},
            {"GPUWidgets/ScanBlockKernel", program<ScanBlockKernel>()},
            {"GPUWidgets/ScanAddKernel", program<ScanAddKernel>()},
            {"GPUWidgets/CoverageShader", program<CoverageShader>()},
            {"GPUWidgets/PathFillShader", program<PathFillShader>()},
            {"GPUWidgets/VertexColorShader", program<VertexColorShader>()},
        });
}

struct SamplingVariant
{
    const char* name;
    GPU::TextureSampling sampling;
};

// All four, because SpriteRenderer builds one program per configuration and
// the sampler is part of the emitted text.
const std::vector<SamplingVariant>& samplingVariants()
{
    static const auto all = std::vector<SamplingVariant> {
        {"NearestClamp", {TextureFilter::Nearest, TextureAddressMode::Clamp}},
        {"NearestRepeat", {TextureFilter::Nearest, TextureAddressMode::Repeat}},
        {"LinearClamp", {TextureFilter::Linear, TextureAddressMode::Clamp}},
        {"LinearRepeat", {TextureFilter::Linear, TextureAddressMode::Repeat}},
    };

    return all;
}

void addSpriteShaders(std::vector<Entry>& entries)
{
    using namespace Sprites;

    for (const auto& variant: samplingVariants())
    {
        entries.push_back({std::string("Sprites/SpriteShader.") + variant.name,
                           program<SpriteShader>(variant.sampling)});
        entries.push_back({std::string("Sprites/Nv12Shader.") + variant.name,
                           program<Nv12Shader>(variant.sampling)});
    }
}

void addRendererShaders(std::vector<Entry>& entries)
{
    entries.insert(
        entries.end(),
        {
            {"UI/ShapeBatch", walked(UI::ShapeBatch::forEachShaderGraph, 0)},
            {"UI/MeshBatch", walked(UI::MeshBatch::forEachShaderGraph, 0)},
            {"UI/ImageBatch", walked(UI::ImageBatch::forEachShaderGraph, 0)},
            {"UI/LayerRenderer", walked(UI::LayerRenderer::forEachShaderGraph, 0)},
            {"Text/GlyphRenderer.Mask",
             walked(Text::GlyphRenderer::forEachShaderGraph, 0)},
            {"Text/GlyphRenderer.Color",
             walked(Text::GlyphRenderer::forEachShaderGraph, 1)},
        });
}

std::vector<Entry> libraryShaders()
{
    auto entries = std::vector<Entry> {};
    addMLKernels(entries);
    addGPUWidgetsShaders(entries);
    addSpriteShaders(entries);
    addRendererShaders(entries);
    return entries;
}

const auto registered =
    registerCorpus("ShaderGolden", EACP_SHADER_GOLDEN_DIR, libraryShaders());
} // namespace
