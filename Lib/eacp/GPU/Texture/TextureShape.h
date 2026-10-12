#pragma once

#include "MipChain.h"
#include "Texture.h"

#include <eacp/Core/Utils/Containers.h>

#include <memory>
#include <optional>

namespace eacp::GPU
{
// What a TextureDescriptor resolves to once its kind, sizes and levels have
// been checked against each other: the part of a texture's validity every
// backend shares. A backend adds only what its device decides (sample counts,
// block compression, typed stores, cube arrays).
struct TextureShape
{
    TextureType kind = TextureType::TwoD;
    int width = 0;
    int height = 0;
    int depth = 1;
    int layers = 1;
    int levels = 1;

    // The levels are the caller's or the GPU's: never built by eacp.
    bool explicitLevels = false;

    bool isCubeKind() const;

    bool isArrayKind() const;

    bool isVolume() const;

    int slices() const;

    int levelWidth(int level) const;
    int levelHeight(int level) const;
    int levelDepth(int level) const;

    bool buildsChain() const;
};

enum class InitialData
{
    None,
    Pixels,
    Subresources
};

int maxMipLevels(TextureType kind, int width, int height, int depth);

std::optional<TextureShape> resolveTextureShape(const TextureDescriptor& descriptor,
                                                InitialData data);

// One box of one level of one slice, with the source it is copied from: the
// unit every backend's upload records.
struct SubresourceUpload
{
    int level = 0;
    int slice = 0;
    int x = 0;
    int y = 0;
    int z = 0;
    int width = 0;
    int height = 0;
    int depth = 1;
    const void* pixels = nullptr;
    int bytesPerRow = 0;
    int bytesPerImage = 0;
};

// Uploads and the chains eacp built for them, which the uploads point into.
struct TextureUploadPlan
{
    Vector<SubresourceUpload> uploads;
    Vector<std::unique_ptr<MipChain>> chains;
    bool valid = true;
};

TextureUploadPlan planWholeUpload(const TextureShape& shape,
                                  TextureFormat format,
                                  const void* pixels,
                                  int bytesPerRow);

TextureUploadPlan
    planSubresourceUploads(const TextureShape& shape,
                           TextureFormat format,
                           std::span<const TextureSubresource> subresources);

std::optional<SubresourceUpload> planRegionUpload(const TextureShape& shape,
                                                  TextureFormat format,
                                                  const TextureRegion& region,
                                                  const void* pixels);

std::optional<SubresourceUpload> planRegionRead(const TextureShape& shape,
                                                TextureFormat format,
                                                const TextureRegion& region,
                                                void* dst);
} // namespace eacp::GPU
