#pragma once

namespace eacp::SA3Codec
{
enum class CodecAttentionMode
{
    ChunkMidpointShift,
    SlidingWindow
};

struct CodecConfig
{
    int channels = 0;
    int transformerDim = 0;
    int transformerDepth = 0;
    int dimHeads = 64;
    int stride = 16;
    int patchChannels = 512;
    int decoderMappingKernelSize = 3;
    int decoderSinusoidalBlockCount = 0;
    CodecAttentionMode attentionMode = CodecAttentionMode::ChunkMidpointShift;
    int chunkSize = 32;
    int slidingWindowRadiusChunks = 1;

    int heads() const
    {
        return transformerDim / dimHeads;
    }

    static CodecConfig sameS()
    {
        return CodecConfig {.channels = 128,
                            .transformerDim = 768,
                            .transformerDepth = 6,
                            .dimHeads = 64,
                            .stride = 16,
                            .patchChannels = 512,
                            .decoderMappingKernelSize = 3,
                            .decoderSinusoidalBlockCount = 0,
                            .attentionMode = CodecAttentionMode::ChunkMidpointShift,
                            .chunkSize = 32,
                            .slidingWindowRadiusChunks = 0};
    }

    static CodecConfig sameL()
    {
        return CodecConfig {.channels = 256,
                            .transformerDim = 1536,
                            .transformerDepth = 12,
                            .dimHeads = 64,
                            .stride = 16,
                            .patchChannels = 512,
                            .decoderMappingKernelSize = 1,
                            .decoderSinusoidalBlockCount = 8,
                            .attentionMode = CodecAttentionMode::SlidingWindow,
                            .chunkSize = 32,
                            .slidingWindowRadiusChunks = 1};
    }
};
}
