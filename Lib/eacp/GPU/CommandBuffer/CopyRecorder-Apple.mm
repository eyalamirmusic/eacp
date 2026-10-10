#import <Metal/Metal.h>

#include "CopyRecorder.h"

namespace eacp::GPU
{
namespace
{
id<MTLTexture> metalTextureOf(const Texture& texture)
{
    return (__bridge id<MTLTexture>) texture.nativeTexture();
}

id<MTLBuffer> metalBufferOf(const Buffer& buffer)
{
    return (__bridge id<MTLBuffer>) buffer.nativeBuffer();
}

MTLOrigin originOf(int x, int y)
{
    return MTLOriginMake((NSUInteger) x, (NSUInteger) y, 0);
}

MTLSize sizeOf(int width, int height)
{
    return MTLSizeMake((NSUInteger) width, (NSUInteger) height, 1);
}

template <typename Encode>
void withBlitEncoder(void* recording, Encode&& encode)
{
    auto commandBuffer = (__bridge id<MTLCommandBuffer>) recording;

    if (commandBuffer == nil)
        return;

    auto blit = [commandBuffer blitCommandEncoder];
    encode(blit);
    [blit endEncoding];
}
} // namespace

void recordCopy(void* recording, const TextureCopyPlan& plan)
{
    auto source = metalTextureOf(*plan.source);
    auto destination = metalTextureOf(*plan.destination);

    if (source == nil || destination == nil)
        return;

    // One blit per slice: the box form takes one slice, and a volume's
    // images are the box's depth.
    withBlitEncoder(
        recording,
        [&](id<MTLBlitCommandEncoder> blit)
        {
            for (auto slice = 0; slice < plan.sliceCount; ++slice)
                [blit copyFromTexture:source
                             sourceSlice:(NSUInteger) (plan.sourceSlice + slice)
                             sourceLevel:(NSUInteger) plan.sourceLevel
                            sourceOrigin:MTLOriginMake((NSUInteger) plan.sourceX,
                                                       (NSUInteger) plan.sourceY,
                                                       (NSUInteger) plan.sourceZ)
                              sourceSize:MTLSizeMake((NSUInteger) plan.width,
                                                     (NSUInteger) plan.height,
                                                     (NSUInteger) plan.depth)
                               toTexture:destination
                        destinationSlice:(NSUInteger) (plan.destinationSlice + slice)
                        destinationLevel:(NSUInteger) plan.destinationLevel
                       destinationOrigin:MTLOriginMake((NSUInteger) plan.destinationX,
                                                       (NSUInteger) plan.destinationY,
                                                       (NSUInteger) plan.destinationZ)];
        });
}

void recordCopy(void* recording, const BufferCopyPlan& plan)
{
    auto source = metalBufferOf(*plan.source);
    auto destination = metalBufferOf(*plan.destination);

    if (source == nil || destination == nil)
        return;

    withBlitEncoder(recording,
                    [&](id<MTLBlitCommandEncoder> blit)
                    {
                        [blit copyFromBuffer:source
                                 sourceOffset:(NSUInteger) plan.sourceOffset
                                     toBuffer:destination
                            destinationOffset:(NSUInteger) plan.destinationOffset
                                         size:(NSUInteger) plan.bytes];
                    });
}

void recordCopyToTexture(void* recording, const BufferTextureCopyPlan& plan)
{
    auto buffer = metalBufferOf(*plan.buffer);
    auto texture = metalTextureOf(*plan.texture);

    if (buffer == nil || texture == nil)
        return;

    withBlitEncoder(
        recording,
        [&](id<MTLBlitCommandEncoder> blit)
        {
            [blit copyFromBuffer:buffer
                       sourceOffset:(NSUInteger) plan.bufferOffset
                  sourceBytesPerRow:(NSUInteger) plan.bytesPerRow
                sourceBytesPerImage:(NSUInteger) plan.bytesPerRow
                                    * (NSUInteger) plan.height
                         sourceSize:sizeOf(plan.width, plan.height)
                          toTexture:texture
                   destinationSlice:(NSUInteger) plan.slice
                   destinationLevel:(NSUInteger) plan.level
                  destinationOrigin:originOf(plan.x, plan.y)];
        });
}

void recordCopyToBuffer(void* recording, const BufferTextureCopyPlan& plan)
{
    auto buffer = metalBufferOf(*plan.buffer);
    auto texture = metalTextureOf(*plan.texture);

    if (buffer == nil || texture == nil)
        return;

    withBlitEncoder(
        recording,
        [&](id<MTLBlitCommandEncoder> blit)
        {
            [blit copyFromTexture:texture
                             sourceSlice:(NSUInteger) plan.slice
                             sourceLevel:(NSUInteger) plan.level
                            sourceOrigin:originOf(plan.x, plan.y)
                              sourceSize:sizeOf(plan.width, plan.height)
                                toBuffer:buffer
                       destinationOffset:(NSUInteger) plan.bufferOffset
                  destinationBytesPerRow:(NSUInteger) plan.bytesPerRow
                destinationBytesPerImage:(NSUInteger) plan.bytesPerRow
                                         * (NSUInteger) plan.height];
        });
}

// Every slice, cube face and volume image at once; Metal filters each level
// from the one above it.
void recordMipGeneration(void* recording, const Texture& texture)
{
    auto metalTexture = metalTextureOf(texture);

    if (metalTexture == nil)
        return;

    withBlitEncoder(recording,
                    [&](id<MTLBlitCommandEncoder> blit)
                    { [blit generateMipmapsForTexture:metalTexture]; });
}
} // namespace eacp::GPU
