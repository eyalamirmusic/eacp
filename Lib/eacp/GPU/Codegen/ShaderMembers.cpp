#include "ShaderMembers.h"
#include "UniformLayout.h"

#include <cstring>

namespace eacp::GPU
{
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

void ShaderVisitor::operator()(const char* name, Uniform<AtomicBuffer>& member)
{
    onAtomicBuffer(name, member, member.value);
}

void ShaderVisitor::operator()(const char* name, Uniform<WritableTexture2D>& member)
{
    onWritableTexture(name, member, member.value);
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

void ShaderVisitor::onAtomicBuffer(const char*, AtomicBuffer&, const BufferRange&) {}

void ShaderVisitor::onWritableTexture(const char*,
                                      WritableTexture2D&,
                                      const Texture*)
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

void ShaderBuildVisitor::onAtomicBuffer(const char*,
                                        AtomicBuffer& handle,
                                        const BufferRange&)
{
    handle = builder.atomicBuffer();
}

void ShaderBuildVisitor::onWritableTexture(const char*,
                                           WritableTexture2D& handle,
                                           const Texture*)
{
    handle = builder.writableTexture();
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
