#include "ShaderProgram.h"

#include <cassert>
#include <cmath>

namespace eacp::GPU
{
ShaderTextureBindVisitor::ShaderTextureBindVisitor(RenderPass& passToUse)
    : pass(passToUse)
{
}

void ShaderTextureBindVisitor::onUniform(const char*,
                                         ValueType,
                                         detail::ValueHandle&,
                                         const void*)
{
}

void ShaderTextureBindVisitor::onTexture(const char*,
                                         Texture2D& handle,
                                         const Texture* texture,
                                         TextureSampling sampling)
{
    if (texture != nullptr)
        pass.setFragmentTexture(*texture, handle.slot, sampling);
}

void ShaderTextureBindVisitor::onCubeTexture(const char*,
                                             TextureCube& handle,
                                             const Texture* texture,
                                             TextureSampling sampling)
{
    if (texture != nullptr)
        pass.setFragmentTexture(*texture, handle.slot, sampling);
}

void ShaderTextureBindVisitor::onDepthTexture(const char*,
                                              TextureDepth2D& handle,
                                              const Texture* renderTarget,
                                              TextureSampling sampling)
{
    if (renderTarget != nullptr)
        pass.setFragmentDepthTexture(*renderTarget, handle.slot, sampling);
}

ShaderBufferBindVisitor::ShaderBufferBindVisitor(RenderPass& passToUse)
    : pass(passToUse)
{
}

void ShaderBufferBindVisitor::onUniform(const char*,
                                        ValueType,
                                        detail::ValueHandle&,
                                        const void*)
{
}

void ShaderBufferBindVisitor::onInputBuffer(const char*,
                                            InputBuffer& handle,
                                            const BufferRange& range)
{
    if (!range.isValid())
        return;

    pass.setVertexStorageBuffer(range, handle.slot);
    pass.setFragmentStorageBuffer(range, handle.slot);
}

void ShaderBufferBindVisitor::onUIntInputBuffer(const char*,
                                                UIntInputBuffer& handle,
                                                const BufferRange& range)
{
    if (!range.isValid())
        return;

    assert(range.offset == 0
           && "eacp: a render program's Uniform<UIntInputBuffer> binds the "
              "whole buffer - RenderPass has no ranged storage bind");

    pass.setVertexStorageBuffer(*range.buffer, handle.slot);
    pass.setFragmentStorageBuffer(*range.buffer, handle.slot);
}

void ShaderBufferBindVisitor::onOutputBuffer(const char*,
                                             OutputBuffer&,
                                             const BufferRange&)
{
    assert(false
           && "eacp: a render program cannot write a buffer - "
              "Uniform<OutputBuffer> belongs to a ComputeProgram");
}

void ShaderBufferBindVisitor::onUIntOutputBuffer(const char*,
                                                 UIntOutputBuffer&,
                                                 const BufferRange&)
{
    assert(false
           && "eacp: a render program cannot write a buffer - "
              "Uniform<UIntOutputBuffer> belongs to a ComputeProgram");
}

void ShaderBufferBindVisitor::onAtomicBuffer(const char*,
                                             AtomicBuffer&,
                                             const BufferRange&)
{
    assert(false
           && "eacp: a render program cannot write a buffer - "
              "Uniform<AtomicBuffer> belongs to a ComputeProgram");
}

void ShaderBufferBindVisitor::onWritableTexture(const char*,
                                                WritableTexture2D&,
                                                const Texture*)
{
    assert(false
           && "eacp: a render program cannot write a texture - "
              "Uniform<WritableTexture2D> belongs to a ComputeProgram");
}

ShaderProgram::ShaderProgram() = default;

ShaderProgram::~ShaderProgram() = default;

const ShaderSource& ShaderProgram::source() const
{
    return generated.source;
}

const VertexLayout& ShaderProgram::vertexLayout() const
{
    return generated.vertexLayout;
}

const ShaderGraph& ShaderProgram::graph() const
{
    return builder.graph();
}

void ShaderProgram::setIndices(const std::uint32_t* data, int count)
{
    uploadIndices(data, (int) sizeof(std::uint32_t), count, IndexFormat::UInt32);
}

void ShaderProgram::setIndices(const std::uint16_t* data, int count)
{
    uploadIndices(data, (int) sizeof(std::uint16_t), count, IndexFormat::UInt16);
}

void ShaderProgram::setInstanceBuffer(int bufferIndex,
                                      const Buffer& buffer,
                                      int count)
{
    assert(bufferIndex >= 0 && bufferIndex < vertexLayout().buffers.size()
           && "instance buffer slot was not declared via instanceInput");

    setExternalInstanceBuffer(bufferIndex, &buffer);
    instanceCountValue = count;
}

void ShaderProgram::prepare(int sampleCount,
                            bool depth,
                            PrimitiveTopology topology,
                            BlendMode blendMode,
                            PixelFormat colorFormat)
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = sampleCount;
    descriptor.depth = depth;
    descriptor.topology = topology;
    descriptor.blendMode = blendMode;
    descriptor.colorFormat = colorFormat;

    prepare(descriptor);
}

void ShaderProgram::prepare(RenderPipelineDescriptor descriptor)
{
    shaderLibrary.emplace(Device::shared(), generated.source);

    descriptor.library = &*shaderLibrary;
    descriptor.vertexLayout = generated.vertexLayout;

    pipelineState.emplace(Device::shared(), descriptor);
}

const RenderPipeline& ShaderProgram::pipeline() const
{
    return *pipelineState;
}

const Buffer& ShaderProgram::vertices() const
{
    return *vertexBufferData;
}

bool ShaderProgram::hasVertices() const
{
    return vertexBufferData.has_value();
}

bool ShaderProgram::hasIndices() const
{
    return indexBufferData.has_value();
}

const Buffer& ShaderProgram::indices() const
{
    return *indexBufferData;
}

const void* ShaderProgram::packedUniforms()
{
    packUniforms();
    return uniformBytes.data();
}

int ShaderProgram::uniformByteSize() const
{
    return uniformBytes.size();
}

bool ShaderProgram::hasUniforms() const
{
    return !uniformBytes.empty();
}

bool ShaderProgram::vertexReadsUniforms() const
{
    return generated.vertexReadsUniforms;
}

bool ShaderProgram::fragmentReadsUniforms() const
{
    return generated.fragmentReadsUniforms;
}

void ShaderProgram::bindTextures(RenderPass& pass)
{
    auto bindVisitor = ShaderTextureBindVisitor {pass};
    reflectMembers(bindVisitor);
}

void ShaderProgram::bindBuffers(RenderPass& pass)
{
    auto bindVisitor = ShaderBufferBindVisitor {pass};
    reflectMembers(bindVisitor);
}

void ShaderProgram::bindInstances(RenderPass& pass)
{
    // Over both lists: a program fed only by kernels has no owned uploads at
    // all, so bounding this by instanceBuffers alone would bind nothing and
    // leave the draw missing its per-instance stream.
    auto slots = std::max(instanceBuffers.size(), externalInstanceBuffers.size());

    for (auto slot = 0; slot < slots; ++slot)
        if (auto range = instanceBufferAt(slot); range.buffer != nullptr)
            pass.setVertexBuffer(range, slot);
}

void ShaderProgram::compile()
{
    auto buildVisitor = ShaderBuildVisitor {builder};
    reflectMembers(buildVisitor);
    define();
    generated = builder.build();

    // define() assembled the vertex layout from the pulled fields' real
    // offsets; use it when any input was pulled.
    if (vertexLayoutData.attributes.size() > 0)
    {
        // instanceInput populated the per-instance slots; publish the
        // per-vertex slot 0 too so every bound buffer carries a stride and step
        // rate. Single-buffer programs keep the pre-instancing shape (empty
        // buffers + stride) untouched.
        if (usesInstancing)
            vertexLayoutData.buffer(0, vertexLayoutData.stride, StepRate::PerVertex);

        generated.vertexLayout = vertexLayoutData;
    }

    packUniforms();
}

Float ShaderProgram::varying(const Float& vertexValue)
{
    return builder.varying(vertexValue);
}

Float2 ShaderProgram::varying(const Float2& vertexValue)
{
    return builder.varying(vertexValue);
}

Float3 ShaderProgram::varying(const Float3& vertexValue)
{
    return builder.varying(vertexValue);
}

Float4 ShaderProgram::varying(const Float4& vertexValue)
{
    return builder.varying(vertexValue);
}

Float ShaderProgram::constant(float value)
{
    return builder.constant(value);
}

Bool ShaderProgram::boolean(bool value)
{
    return builder.boolean(value);
}

Int ShaderProgram::integer(int value)
{
    return builder.integer(value);
}

UInt ShaderProgram::unsignedInteger(unsigned value)
{
    return builder.unsignedInteger(value);
}

Var<Float> ShaderProgram::var(float initialValue)
{
    return builder.var(initialValue);
}

Var<Bool> ShaderProgram::var(bool initialValue)
{
    return builder.var(initialValue);
}

Var<Int> ShaderProgram::var(int initialValue)
{
    return builder.var(initialValue);
}

Var<UInt> ShaderProgram::var(unsigned initialValue)
{
    return builder.var(initialValue);
}

void ShaderProgram::breakLoop()
{
    builder.breakLoop();
}

void ShaderProgram::continueLoop()
{
    builder.continueLoop();
}

Float4x4 ShaderProgram::translate(float x, float y, float z)
{
    auto o = constant(1.0f);
    auto z0 = constant(0.0f);
    return float4x4(float4(o, z0, z0, z0),
                    float4(z0, o, z0, z0),
                    float4(z0, z0, o, z0),
                    float4(constant(x), constant(y), constant(z), o));
}

Float4x4 ShaderProgram::translate(const Float& x, const Float& y, const Float& z)
{
    auto o = constant(1.0f);
    auto z0 = constant(0.0f);
    return float4x4(float4(o, z0, z0, z0),
                    float4(z0, o, z0, z0),
                    float4(z0, z0, o, z0),
                    float4(x, y, z, o));
}

Float4x4 ShaderProgram::rotateY(const Float& angle)
{
    auto c = cos(angle);
    auto s = sin(angle);
    auto z0 = constant(0.0f);
    auto o = constant(1.0f);
    return float4x4(float4(c, z0, -s, z0),
                    float4(z0, o, z0, z0),
                    float4(s, z0, c, z0),
                    float4(z0, z0, z0, o));
}

Float4x4 ShaderProgram::rotateZ(const Float& angle)
{
    auto c = cos(angle);
    auto s = sin(angle);
    auto z0 = constant(0.0f);
    auto o = constant(1.0f);
    return float4x4(float4(c, s, z0, z0),
                    float4(-s, c, z0, z0),
                    float4(z0, z0, o, z0),
                    float4(z0, z0, z0, o));
}

Float4x4 ShaderProgram::rotateX(float radians)
{
    auto c = constant(std::cos(radians));
    auto s = constant(std::sin(radians));
    auto z0 = constant(0.0f);
    auto o = constant(1.0f);
    return float4x4(float4(o, z0, z0, z0),
                    float4(z0, c, s, z0),
                    float4(z0, -s, c, z0),
                    float4(z0, z0, z0, o));
}

Float4x4 ShaderProgram::perspective(const Float& aspect,
                                    float fovY,
                                    float nearZ,
                                    float farZ)
{
    auto f = constant(1.0f / std::tan(fovY * 0.5f));
    auto z0 = constant(0.0f);
    return float4x4(float4(f / aspect, z0, z0, z0),
                    float4(z0, f, z0, z0),
                    float4(z0, z0, constant(farZ / (nearZ - farZ)), constant(-1.0f)),
                    float4(z0, z0, constant((farZ * nearZ) / (nearZ - farZ)), z0));
}

void ShaderProgram::setPosition(const Float4& clipPosition)
{
    builder.position(clipPosition);
}

void ShaderProgram::setFragment(const Float4& color)
{
    builder.fragment(color);
}

void ShaderProgram::setDiscardBelow(const Float& value, float threshold)
{
    builder.discardBelow(value, threshold);
}

void ShaderProgram::packUniforms()
{
    uniformBytes.clear();
    auto uploadVisitor = ShaderUploadVisitor {uniformBytes};
    reflectMembers(uploadVisitor);
    uploadVisitor.finish();
}

void ShaderProgram::setExternalInstanceBuffer(int bufferIndex, const Buffer* buffer)
{
    if (externalInstanceBuffers.size() <= bufferIndex)
        externalInstanceBuffers.resize(bufferIndex + 1);

    externalInstanceBuffers[bufferIndex] = buffer;
}

BufferRange ShaderProgram::instanceBufferAt(int slot) const
{
    if (slot < externalInstanceBuffers.size()
        && externalInstanceBuffers[slot] != nullptr)
        return BufferRange::of(*externalInstanceBuffers[slot]);

    if (slot < instanceBuffers.size())
        return instanceBuffers[slot];

    return {};
}

void ShaderProgram::uploadIndices(const void* data,
                                  int elementSize,
                                  int count,
                                  IndexFormat format)
{
    indexBufferData.emplace(Device::shared(),
                            data,
                            (std::int64_t) elementSize * count,
                            BufferUsage::Index);
    indexCountValue = count;
    indexFormatValue = format;
}
} // namespace eacp::GPU
