#pragma once

#include "TextureFunctions.h"

#include <type_traits>

// WGSL's storage textures: texture_storage_2d, texture_storage_2d_array and
// texture_storage_3d in a format and an access mode, read with textureLoad and
// written with textureStore. The format is part of the type, as in WGSL: it
// decides what a load gives and a store takes - a Float4, a UInt4 or an Int4,
// four components whatever the format holds - and what each dialect declares.
//
//     Uniform<StorageTexture2D<TextureFormat::R32Float, StorageAccess::ReadWrite>>
//         heights;
//
//     auto p = int2(toInt(threadPosition().x), toInt(threadPosition().y));
//     write(heights, p, load(heights, p) * 0.5f);
//
// Write and ReadWrite bind through ComputePass::setOutputTexture (a texture
// created with computeWrite); Read through setInputTexture, in a kernel or a
// fragment shader. See "Storage textures" in Lib/eacp/GPU/README.md for the
// capability each access needs.

namespace eacp::GPU
{
enum class StorageAccess
{
    Write,
    Read,
    ReadWrite
};

constexpr TextureAccess textureAccessOf(StorageAccess access)
{
    switch (access)
    {
        case StorageAccess::Read:
            return TextureAccess::Read;
        case StorageAccess::ReadWrite:
            return TextureAccess::ReadWrite;
        case StorageAccess::Write:
            break;
    }

    return TextureAccess::Write;
}

constexpr bool canLoad(StorageAccess access)
{
    return access != StorageAccess::Write;
}

constexpr bool canStore(StorageAccess access)
{
    return access != StorageAccess::Read;
}

// What a storage slot is, as the member walk hands it over.
struct StorageTextureLayout
{
    TextureKind kind = TextureKind::Texture2D;
    TextureFormat format = TextureFormat::RGBA8Unorm;
    TextureAccess access = TextureAccess::Write;
};

template <TextureKind Kind, TextureFormat Format, StorageAccess Access>
struct StorageTexture : detail::TextureHandle
{
    static_assert(Kind == TextureKind::Texture2D
                      || Kind == TextureKind::Texture2DArray
                      || Kind == TextureKind::Texture3D,
                  "eacp: a storage texture is 2D, a 2D array or 3D");

    static_assert(isStorageTextureFormat(Format),
                  "eacp: no storage texture in BGRA8Unorm, an sRGB, a block or "
                  "the RGB9E5Float format - a kernel cannot store to one");

    static constexpr auto layout =
        StorageTextureLayout {Kind, Format, textureAccessOf(Access)};
};

template <TextureFormat Format, StorageAccess Access = StorageAccess::Write>
using StorageTexture2D = StorageTexture<TextureKind::Texture2D, Format, Access>;

template <TextureFormat Format, StorageAccess Access = StorageAccess::Write>
using StorageTexture2DArray =
    StorageTexture<TextureKind::Texture2DArray, Format, Access>;

template <TextureFormat Format, StorageAccess Access = StorageAccess::Write>
using StorageTexture3D = StorageTexture<TextureKind::Texture3D, Format, Access>;

// The texel a storage texture of this format loads and stores.
template <TextureFormat Format>
using StorageTexelOf = std::conditional_t<
    storageTexelType(Format) == ValueType::UInt4,
    UInt4,
    std::conditional_t<storageTexelType(Format) == ValueType::Int4, Int4, Float4>>;

namespace detail
{
template <typename Result>
Result storageLoad(const TextureHandle& texture, int coordinates, int layer)
{
    auto result = Result {};
    result.graph = texture.graph;
    result.node = texture.graph->addStorageLoad(texture.slot, coordinates, layer);
    return result;
}

inline void storeStorageTexel(const TextureHandle& texture,
                              int coordinates,
                              int layer,
                              int value)
{
    texture.graph->addStorageStore(texture.slot, coordinates, layer, value);
}
} // namespace detail

// textureLoad on a Read or ReadWrite storage texture. A one-, two- or
// three-channel format fills the rest as (0, 0, 1). An address outside the
// texture is the caller's bug, as for load() on a sampled kind.
template <TextureFormat Format, StorageAccess Access>
    requires(canLoad(Access))
StorageTexelOf<Format> load(const StorageTexture2D<Format, Access>& texture,
                            const TexelCoordinates2& coordinates)
{
    return detail::storageLoad<StorageTexelOf<Format>>(
        texture, coordinates.node, -1);
}

template <TextureFormat Format, StorageAccess Access>
    requires(canLoad(Access))
StorageTexelOf<Format> load(const StorageTexture2DArray<Format, Access>& texture,
                            const TexelCoordinates2& coordinates,
                            const TextureIndex& layer)
{
    return detail::storageLoad<StorageTexelOf<Format>>(
        texture, coordinates.node, layer.nodeOn(*texture.graph));
}

template <TextureFormat Format, StorageAccess Access>
    requires(canLoad(Access))
StorageTexelOf<Format> load(const StorageTexture3D<Format, Access>& texture,
                            const TexelCoordinates3& coordinates)
{
    return detail::storageLoad<StorageTexelOf<Format>>(
        texture, coordinates.node, -1);
}

// textureDimensions (a UInt2, a UInt3 on a volume) and textureNumLayers.
template <TextureKind Kind, TextureFormat Format, StorageAccess Access>
std::conditional_t<Kind == TextureKind::Texture3D, UInt3, UInt2>
    dimensions(const StorageTexture<Kind, Format, Access>& texture)
{
    using Size = std::conditional_t<Kind == TextureKind::Texture3D, UInt3, UInt2>;
    return detail::textureQuery<Size>(texture.graph, texture.slot, "size", -1);
}

template <TextureFormat Format, StorageAccess Access>
UInt layerCount(const StorageTexture2DArray<Format, Access>& texture)
{
    return detail::textureQuery<UInt>(texture.graph, texture.slot, "layers", -1);
}
} // namespace eacp::GPU
