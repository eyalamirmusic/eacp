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

void ShaderVisitor::onAtomicBuffer(const char*, AtomicBuffer&, const BufferRange&) {}

void ShaderVisitor::onIntAtomicBuffer(const char*,
                                      IntAtomicBuffer&,
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

void ShaderBuildVisitor::onUniform(const char*,
                                   ValueType type,
                                   detail::ValueHandle& handle,
                                   const void*)
{
    handle = builder.addUniform(type);
}

void ShaderBuildVisitor::onTexture(const char*,
                                   Texture2D& handle,
                                   const Texture*,
                                   TextureSampling sampling)
{
    handle = builder.texture(sampling);
}

void ShaderBuildVisitor::onCubeTexture(const char*,
                                       TextureCube& handle,
                                       const Texture*,
                                       TextureSampling sampling)
{
    handle = builder.cubeTexture(sampling);
}

void ShaderBuildVisitor::onDepthTexture(const char*,
                                        TextureDepth2D& handle,
                                        const Texture*,
                                        TextureSampling sampling)
{
    handle = builder.depthTexture(sampling);
}

void ShaderBuildVisitor::onArrayTexture(const char*,
                                        Texture2DArray& handle,
                                        const Texture*,
                                        TextureSampling sampling)
{
    handle = builder.textureArray(sampling);
}

void ShaderBuildVisitor::onDepthArrayTexture(const char*,
                                             TextureDepth2DArray& handle,
                                             const Texture*,
                                             TextureSampling sampling)
{
    handle = builder.depthTextureArray(sampling);
}

void ShaderBuildVisitor::onTextureOfKind(const char*,
                                         TextureKind kind,
                                         detail::TextureHandle& handle,
                                         const Texture*,
                                         TextureSampling sampling)
{
    handle = builder.textureOfKind(kind, sampling);
}

void ShaderBuildVisitor::onInputBuffer(const char*,
                                       InputBuffer& handle,
                                       const BufferRange&)
{
    handle = builder.inputBuffer();
}

void ShaderBuildVisitor::onOutputBuffer(const char*,
                                        OutputBuffer& handle,
                                        const BufferRange&)
{
    handle = builder.outputBuffer();
}

void ShaderBuildVisitor::onUIntInputBuffer(const char*,
                                           UIntInputBuffer& handle,
                                           const BufferRange&)
{
    handle = builder.uintInputBuffer();
}

void ShaderBuildVisitor::onUIntOutputBuffer(const char*,
                                            UIntOutputBuffer& handle,
                                            const BufferRange&)
{
    handle = builder.uintOutputBuffer();
}

void ShaderBuildVisitor::onIntInputBuffer(const char*,
                                          IntInputBuffer& handle,
                                          const BufferRange&)
{
    handle = builder.intInputBuffer();
}

void ShaderBuildVisitor::onIntOutputBuffer(const char*,
                                           IntOutputBuffer& handle,
                                           const BufferRange&)
{
    handle = builder.intOutputBuffer();
}

void ShaderBuildVisitor::onAtomicBuffer(const char*,
                                        AtomicBuffer& handle,
                                        const BufferRange&)
{
    handle = builder.atomicBuffer();
}

void ShaderBuildVisitor::onIntAtomicBuffer(const char*,
                                           IntAtomicBuffer& handle,
                                           const BufferRange&)
{
    handle = builder.intAtomicBuffer();
}

void ShaderBuildVisitor::onWritableTexture(const char*,
                                           WritableTexture2D& handle,
                                           const Texture*)
{
    handle = builder.writableTexture();
}

void ShaderBuildVisitor::onStorageTexture(const char*,
                                          const StorageTextureLayout& layout,
                                          detail::TextureHandle& handle,
                                          const Texture*)
{
    handle = builder.storageTextureOf(layout);
}

void ShaderBuildVisitor::onSampler(const char*,
                                   detail::SamplerHandle& handle,
                                   bool comparison,
                                   const Sampler&)
{
    handle = builder.samplerOf(comparison);
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
