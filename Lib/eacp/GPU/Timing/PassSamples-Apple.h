#pragma once

#import <Metal/Metal.h>

// Where a timed pass's samples sit in a Metal counter sample buffer: four to a
// pass, one per stage boundary of a render pass. A compute encoder has only its
// own start and end, so it takes the first and last of its four and leaves the
// two between unwritten - which the resolve tells apart by their being outside
// the pass's own start and end.

namespace eacp::GPU::PassSamples
{
constexpr int perPass = 4;

constexpr int vertexStart = 0;
constexpr int vertexEnd = 1;
constexpr int fragmentStart = 2;
constexpr int fragmentEnd = 3;

inline NSUInteger index(int pass, int boundary)
{
    return (NSUInteger) (pass * perPass + boundary);
}

inline void attach(MTLRenderPassDescriptor* descriptor,
                   id<MTLCounterSampleBuffer> samples,
                   int pass)
{
    auto attachment = descriptor.sampleBufferAttachments[0];

    attachment.sampleBuffer = samples;
    attachment.startOfVertexSampleIndex = index(pass, vertexStart);
    attachment.endOfVertexSampleIndex = index(pass, vertexEnd);
    attachment.startOfFragmentSampleIndex = index(pass, fragmentStart);
    attachment.endOfFragmentSampleIndex = index(pass, fragmentEnd);
}

inline void attach(MTLComputePassDescriptor* descriptor,
                   id<MTLCounterSampleBuffer> samples,
                   int pass)
{
    auto attachment = descriptor.sampleBufferAttachments[0];

    attachment.sampleBuffer = samples;
    attachment.startOfEncoderSampleIndex = index(pass, vertexStart);
    attachment.endOfEncoderSampleIndex = index(pass, fragmentEnd);
}
} // namespace eacp::GPU::PassSamples
