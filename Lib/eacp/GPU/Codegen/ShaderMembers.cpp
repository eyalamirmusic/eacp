#include "ShaderMembers.h"
#include "UniformLayout.h"

#include <cstring>

namespace eacp::GPU
{
namespace
{
// Tightly packed columns on the CPU, one per uniformColumnStride in the block.
void copyMatrixColumns(ValueType type, const void* data, std::byte* block)
{
    const auto* columns = static_cast<const std::byte*>(data);
    auto columnBytes = (std::size_t) matrixRows(type) * sizeof(float);

    for (auto column = 0; column < matrixColumns(type); ++column)
        std::memcpy(block + column * uniformColumnStride,
                    columns + column * columnBytes,
                    columnBytes);
}

template <typename Handle>
void nameBuiltTexture(const char* name, const Handle& handle)
{
    if (handle.graph != nullptr && name != nullptr)
        handle.graph->nameTexture(handle.slot, name);
}

template <typename Handle>
void nameBuiltBuffer(const char* name, const Handle& handle)
{
    if (handle.graph != nullptr && name != nullptr)
        handle.graph->nameStorageBuffer(handle.slot, name);
}
} // namespace

ShaderVisitor::~ShaderVisitor() = default;

void ShaderVisitor::operator()(const char* name, Uniform<Texture2D>& member)
{
    onTexture(name, member, member.value, member.sampling);
}

void ShaderVisitor::operator()(const char* name, Uniform<TextureCube>& member)
{
    onCubeTexture(name, member, member.value, member.sampling);
}

void ShaderVisitor::operator()(const char* name, Uniform<TextureDepth2D>& member)
{
    onDepthTexture(name, member, member.value, member.sampling);
}

void ShaderVisitor::operator()(const char* name, Uniform<Texture2DArray>& member)
{
    onArrayTexture(name, member, member.value, member.sampling);
}

void ShaderVisitor::operator()(const char* name,
                               Uniform<TextureDepth2DArray>& member)
{
    onDepthArrayTexture(name, member, member.value, member.sampling);
}

void ShaderVisitor::operator()(const char* name, Uniform<Texture3D>& member)
{
    onTextureOfKind(
        name, TextureKind::Texture3D, member, member.value, member.sampling);
}

void ShaderVisitor::operator()(const char* name, Uniform<TextureCubeArray>& member)
{
    onTextureOfKind(
        name, TextureKind::CubeArray, member, member.value, member.sampling);
}

void ShaderVisitor::operator()(const char* name, Uniform<TextureDepthCube>& member)
{
    onTextureOfKind(
        name, TextureKind::DepthCube, member, member.value, member.sampling);
}

void ShaderVisitor::operator()(const char* name,
                               Uniform<TextureDepthCubeArray>& member)
{
    onTextureOfKind(
        name, TextureKind::DepthCubeArray, member, member.value, member.sampling);
}

void ShaderVisitor::operator()(const char* name, Uniform<InputBuffer>& member)
{
    onInputBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<OutputBuffer>& member)
{
    onOutputBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<UIntInputBuffer>& member)
{
    onUIntInputBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<UIntOutputBuffer>& member)
{
    onUIntOutputBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<IntInputBuffer>& member)
{
    onIntInputBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<IntOutputBuffer>& member)
{
    onIntOutputBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<HalfInputBuffer>& member)
{
    onHalfInputBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<HalfOutputBuffer>& member)
{
    onHalfOutputBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<AtomicBuffer>& member)
{
    onAtomicBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<IntAtomicBuffer>& member)
{
    onIntAtomicBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<WritableTexture2D>& member)
{
    onWritableTexture(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<SamplerBinding>& member)
{
    onSampler(name, member, false, member.value);
}

void ShaderVisitor::operator()(const char* name,
                               Uniform<ComparisonSamplerBinding>& member)
{
    onSampler(name, member, true, member.value);
}

void ShaderVisitor::onTexture(const char*,
                              Texture2D&,
                              const Texture*,
                              TextureSampling)
{
}

void ShaderVisitor::onCubeTexture(const char*,
                                  TextureCube&,
                                  const Texture*,
                                  TextureSampling)
{
}

void ShaderVisitor::onDepthTexture(const char*,
                                   TextureDepth2D&,
                                   const Texture*,
                                   TextureSampling)
{
}

void ShaderVisitor::onArrayTexture(const char*,
                                   Texture2DArray&,
                                   const Texture*,
                                   TextureSampling)
{
}

void ShaderVisitor::onDepthArrayTexture(const char*,
                                        TextureDepth2DArray&,
                                        const Texture*,
                                        TextureSampling)
{
}

void ShaderVisitor::onTextureOfKind(const char*,
                                    TextureKind,
                                    detail::TextureHandle&,
                                    const Texture*,
                                    TextureSampling)
{
}

void ShaderVisitor::onInputBuffer(const char*, InputBuffer&, const BufferRange&) {}

void ShaderVisitor::onOutputBuffer(const char*, OutputBuffer&, const BufferRange&) {}

void ShaderVisitor::onUIntInputBuffer(const char*,
                                      UIntInputBuffer&,
                                      const BufferRange&)
{
}

void ShaderVisitor::onUIntOutputBuffer(const char*,
                                       UIntOutputBuffer&,
                                       const BufferRange&)
{
}

void ShaderVisitor::onIntInputBuffer(const char*,
                                     IntInputBuffer&,
                                     const BufferRange&)
{
}

void ShaderVisitor::onIntOutputBuffer(const char*,
                                      IntOutputBuffer&,
                                      const BufferRange&)
{
}

void ShaderVisitor::onHalfInputBuffer(const char*,
                                      HalfInputBuffer&,
                                      const BufferRange&)
{
}

void ShaderVisitor::onHalfOutputBuffer(const char*,
                                       HalfOutputBuffer&,
                                       const BufferRange&)
{
}

void ShaderVisitor::onAtomicBuffer(const char*, AtomicBuffer&, const BufferRange&) {}

void ShaderVisitor::onIntAtomicBuffer(const char*,
                                      IntAtomicBuffer&,
                                      const BufferRange&)
{
}

void ShaderVisitor::onStructBuffer(const char*,
                                   BufferAccess,
                                   detail::StructBufferHandle&,
                                   int (*)(ShaderGraph&),
                                   const BufferRange&)
{
}

void ShaderVisitor::onWritableTexture(const char*,
                                      WritableTexture2D&,
                                      const Texture*)
{
}

void ShaderVisitor::onStorageTexture(const char*,
                                     const StorageTextureLayout&,
                                     detail::TextureHandle&,
                                     const Texture*)
{
}

void ShaderVisitor::onSampler(const char*,
                              detail::SamplerHandle&,
                              bool,
                              const Sampler&)
{
}

ShaderBuildVisitor::ShaderBuildVisitor(ShaderBuilder& builderToUse)
    : builder(builderToUse)
{
}

void ShaderBuildVisitor::onUniform(const char* name,
                                   ValueType type,
                                   detail::ValueHandle& handle,
                                   const void*)
{
    handle = builder.addUniform(type);

    if (handle.graph != nullptr && name != nullptr)
        handle.graph->nameUniform(handle.graph->expr(handle.node).index, name);
}

void ShaderBuildVisitor::onTexture(const char* name,
                                   Texture2D& handle,
                                   const Texture*,
                                   TextureSampling sampling)
{
    handle = builder.texture(sampling);
    nameBuiltTexture(name, handle);
}

void ShaderBuildVisitor::onCubeTexture(const char* name,
                                       TextureCube& handle,
                                       const Texture*,
                                       TextureSampling sampling)
{
    handle = builder.cubeTexture(sampling);
    nameBuiltTexture(name, handle);
}

void ShaderBuildVisitor::onDepthTexture(const char* name,
                                        TextureDepth2D& handle,
                                        const Texture*,
                                        TextureSampling sampling)
{
    handle = builder.depthTexture(sampling);
    nameBuiltTexture(name, handle);
}

void ShaderBuildVisitor::onArrayTexture(const char* name,
                                        Texture2DArray& handle,
                                        const Texture*,
                                        TextureSampling sampling)
{
    handle = builder.textureArray(sampling);
    nameBuiltTexture(name, handle);
}

void ShaderBuildVisitor::onDepthArrayTexture(const char* name,
                                             TextureDepth2DArray& handle,
                                             const Texture*,
                                             TextureSampling sampling)
{
    handle = builder.depthTextureArray(sampling);
    nameBuiltTexture(name, handle);
}

void ShaderBuildVisitor::onTextureOfKind(const char* name,
                                         TextureKind kind,
                                         detail::TextureHandle& handle,
                                         const Texture*,
                                         TextureSampling sampling)
{
    handle = builder.textureOfKind(kind, sampling);
    nameBuiltTexture(name, handle);
}

void ShaderBuildVisitor::onInputBuffer(const char* name,
                                       InputBuffer& handle,
                                       const BufferRange&)
{
    handle = builder.inputBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onOutputBuffer(const char* name,
                                        OutputBuffer& handle,
                                        const BufferRange&)
{
    handle = builder.outputBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onUIntInputBuffer(const char* name,
                                           UIntInputBuffer& handle,
                                           const BufferRange&)
{
    handle = builder.uintInputBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onUIntOutputBuffer(const char* name,
                                            UIntOutputBuffer& handle,
                                            const BufferRange&)
{
    handle = builder.uintOutputBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onIntInputBuffer(const char* name,
                                          IntInputBuffer& handle,
                                          const BufferRange&)
{
    handle = builder.intInputBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onIntOutputBuffer(const char* name,
                                           IntOutputBuffer& handle,
                                           const BufferRange&)
{
    handle = builder.intOutputBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onHalfInputBuffer(const char* name,
                                           HalfInputBuffer& handle,
                                           const BufferRange&)
{
    handle = builder.halfInputBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onHalfOutputBuffer(const char* name,
                                            HalfOutputBuffer& handle,
                                            const BufferRange&)
{
    handle = builder.halfOutputBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onAtomicBuffer(const char* name,
                                        AtomicBuffer& handle,
                                        const BufferRange&)
{
    handle = builder.atomicBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onIntAtomicBuffer(const char* name,
                                           IntAtomicBuffer& handle,
                                           const BufferRange&)
{
    handle = builder.intAtomicBuffer();
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onStructBuffer(const char* name,
                                        BufferAccess access,
                                        detail::StructBufferHandle& handle,
                                        int (*declare)(ShaderGraph&),
                                        const BufferRange&)
{
    handle = builder.structBuffer(access, declare);
    nameBuiltBuffer(name, handle);
}

void ShaderBuildVisitor::onWritableTexture(const char* name,
                                           WritableTexture2D& handle,
                                           const Texture*)
{
    handle = builder.writableTexture();
    nameBuiltTexture(name, handle);
}

void ShaderBuildVisitor::onStorageTexture(const char* name,
                                          const StorageTextureLayout& layout,
                                          detail::TextureHandle& handle,
                                          const Texture*)
{
    handle = builder.storageTextureOf(layout);
    nameBuiltTexture(name, handle);
}

void ShaderBuildVisitor::onSampler(const char* name,
                                   detail::SamplerHandle& handle,
                                   bool comparison,
                                   const Sampler&)
{
    handle = builder.samplerOf(comparison);
    nameBuiltTexture(name, handle);
}

ShaderUploadVisitor::ShaderUploadVisitor(Vector<std::byte>& bytesToFill)
    : bytes(bytesToFill)
{
}

void ShaderUploadVisitor::onUniform(const char*,
                                    ValueType type,
                                    detail::ValueHandle&,
                                    const void* data)
{
    auto alignment = uniformAlignment(type);
    auto offset = alignUp(cursor, alignment);
    auto next = offset + uniformSlotStride(type);

    if (bytes.size() < next)
        bytes.resize(next);

    if (isMatrix(type))
        copyMatrixColumns(type, data, bytes.data() + offset);
    else
        std::memcpy(bytes.data() + offset, data, (std::size_t) byteSize(type));

    cursor = next;

    if (alignment > blockAlignment)
        blockAlignment = alignment;
}

void ShaderUploadVisitor::finish()
{
    bytes.resize(alignUp(bytes.size(), blockAlignment));
}
} // namespace eacp::GPU
