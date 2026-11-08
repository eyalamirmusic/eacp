#include "ShaderProgram.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>

namespace eacp::GPU
{
namespace detail
{
bool isCubeType(TextureType type)
{
    return type == TextureType::Cube || type == TextureType::CubeArray;
}

namespace
{
bool fitsStorageShape(TextureKind kind, TextureType type)
{
    if (kind == TextureKind::Texture2DArray)
        return type == TextureType::TwoDArray || isCubeType(type);

    if (kind == TextureKind::Texture3D)
        return type == TextureType::ThreeD;

    return type == TextureType::TwoD;
}
} // namespace

bool fitsStorageMember(const Texture& texture, const StorageTextureLayout& layout)
{
    return texture.isValid() && texture.format() == layout.format
           && fitsStorageShape(layout.kind, texture.kind());
}

bool fitsSamplerMember(const char* name, const Sampler& sampler, bool comparison)
{
    if (!sampler.isValid())
        return false;

    if (sampler.descriptor().compare.has_value() == comparison)
        return true;

    static auto reported = std::atomic_flag {};

    if (!reported.test_and_set())
        LOG("eacp: sampler member '",
            name,
            comparison
                ? "' is a comparison sampler and was assigned a Sampler with "
                  "no CompareFunction"
                : "' is a plain sampler and was assigned a comparison Sampler",
            "; it binds nothing");

    return false;
}
} // namespace detail

namespace
{
// Texture bind walk: hand each assigned texture member to the render pass at
// the slot its handle was declared with.
class ShaderTextureBindVisitor final : public ShaderVisitor
{
public:
    ShaderTextureBindVisitor(RenderPass& passToUse,
                             const GeneratedShader& shaderToUse);

    void onUniform(const char*,
                   ValueType,
                   detail::ValueHandle&,
                   const void*) override;

    void onTexture(const char*,
                   Texture2D& handle,
                   const Texture* texture,
                   TextureSampling sampling) override;

    // The same call, and that is the point rather than an economy: a cube is one
    // texture on one slot of one index space on both backends, so nothing about
    // binding it differs from binding a 2D image. The dimensionality was settled
    // when the texture was created and when the shader was compiled.
    void onCubeTexture(const char*,
                       TextureCube& handle,
                       const Texture* texture,
                       TextureSampling sampling) override;

    // The one member whose bind is a different call, because what was assigned
    // is a render target and what is wanted is the depth buffer inside it.
    void onDepthTexture(const char*,
                        TextureDepth2D& handle,
                        const Texture* renderTarget,
                        TextureSampling sampling) override;

    void onArrayTexture(const char*,
                        Texture2DArray& handle,
                        const Texture* texture,
                        TextureSampling sampling) override;

    // The whole plane: the slice binds stay for a TextureDepth2D over one layer.
    void onDepthArrayTexture(const char*,
                             TextureDepth2DArray& handle,
                             const Texture* renderTarget,
                             TextureSampling sampling) override;

    void onTextureOfKind(const char*,
                         TextureKind kind,
                         detail::TextureHandle& handle,
                         const Texture* texture,
                         TextureSampling sampling) override;

    // A Read storage texture is a sampled one with no filtering, and a cube
    // has no array view on the fragment stage. A render program writing one
    // is not something the render passes bind.
    void onStorageTexture(const char*,
                          const StorageTextureLayout& layout,
                          detail::TextureHandle& handle,
                          const Texture* texture) override;

    void onSampler(const char* name,
                   detail::SamplerHandle& handle,
                   bool comparison,
                   const Sampler& sampler) override;

private:
    // Every slot to the fragment stage, as always, and to the vertex stage too
    // where the vertex function reads it. On D3D12 and Vulkan the two are one
    // binding, so the second bind restates the first.
    void bindTexture(const Texture& texture, int slot, TextureSampling sampling);
    void bindDepthTexture(const Texture& renderTarget,
                          int slot,
                          TextureSampling sampling);

    RenderPass& pass;
    const GeneratedShader& shader;
};

// Storage-buffer bind walk: hand each assigned input-buffer member to the
// render pass at the slot its handle was declared with.
//
// Bound to both stages, for the reason the uniform block is: which stage reads
// the buffer is a property of define(), not of the member, and a stage whose
// generated function never declares it ignores the bind.
class ShaderBufferBindVisitor final : public ShaderVisitor
{
public:
    explicit ShaderBufferBindVisitor(RenderPass& passToUse);

    void onUniform(const char*,
                   ValueType,
                   detail::ValueHandle&,
                   const void*) override;

    void onInputBuffer(const char*,
                       InputBuffer& handle,
                       const BufferRange& range) override;

    // The integer input reads exactly as the float one does: one storage
    // binding, and only the element type the generated stage declares differs.
    void onUIntInputBuffer(const char*,
                           UIntInputBuffer& handle,
                           const BufferRange& range) override;

    void onOutputBuffer(const char*, OutputBuffer&, const BufferRange&) override;

    void onUIntOutputBuffer(const char*,
                            UIntOutputBuffer&,
                            const BufferRange&) override;

    void onIntInputBuffer(const char*,
                          IntInputBuffer& handle,
                          const BufferRange& range) override;

    void onIntOutputBuffer(const char*,
                           IntOutputBuffer&,
                           const BufferRange&) override;

    void onHalfInputBuffer(const char*,
                           HalfInputBuffer& handle,
                           const BufferRange& range) override;

    void onHalfOutputBuffer(const char*,
                            HalfOutputBuffer&,
                            const BufferRange&) override;

    void onAtomicBuffer(const char*, AtomicBuffer&, const BufferRange&) override;

    void onIntAtomicBuffer(const char*,
                           IntAtomicBuffer&,
                           const BufferRange&) override;

    void onStructBuffer(const char*,
                        BufferAccess access,
                        detail::StructBufferHandle& handle,
                        int (*)(ShaderGraph&),
                        const BufferRange& range) override;

    void onWritableTexture(const char*, WritableTexture2D&, const Texture*) override;

private:
    RenderPass& pass;
};

ShaderTextureBindVisitor::ShaderTextureBindVisitor(
    RenderPass& passToUse, const GeneratedShader& shaderToUse)
    : pass(passToUse)
    , shader(shaderToUse)
{
}

void ShaderTextureBindVisitor::bindTexture(const Texture& texture,
                                           int slot,
                                           TextureSampling sampling)
{
    pass.setFragmentTexture(texture, slot, sampling);

    if (shader.vertexReadsTexture(slot))
        pass.setVertexTexture(texture, slot, sampling);
}

void ShaderTextureBindVisitor::bindDepthTexture(const Texture& renderTarget,
                                                int slot,
                                                TextureSampling sampling)
{
    pass.setFragmentDepthTexture(renderTarget, slot, sampling);

    if (shader.vertexReadsTexture(slot))
        pass.setVertexDepthTexture(renderTarget, slot, sampling);
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
        bindTexture(*texture, handle.slot, sampling);
}

void ShaderTextureBindVisitor::onCubeTexture(const char*,
                                             TextureCube& handle,
                                             const Texture* texture,
                                             TextureSampling sampling)
{
    if (texture != nullptr)
        bindTexture(*texture, handle.slot, sampling);
}

void ShaderTextureBindVisitor::onDepthTexture(const char*,
                                              TextureDepth2D& handle,
                                              const Texture* renderTarget,
                                              TextureSampling sampling)
{
    if (renderTarget != nullptr)
        bindDepthTexture(*renderTarget, handle.slot, sampling);
}

void ShaderTextureBindVisitor::onArrayTexture(const char*,
                                              Texture2DArray& handle,
                                              const Texture* texture,
                                              TextureSampling sampling)
{
    if (texture != nullptr)
        bindTexture(*texture, handle.slot, sampling);
}

void ShaderTextureBindVisitor::onDepthArrayTexture(const char*,
                                                   TextureDepth2DArray& handle,
                                                   const Texture* renderTarget,
                                                   TextureSampling sampling)
{
    if (renderTarget != nullptr)
        bindDepthTexture(*renderTarget, handle.slot, sampling);
}

void ShaderTextureBindVisitor::onTextureOfKind(const char*,
                                               TextureKind kind,
                                               detail::TextureHandle& handle,
                                               const Texture* texture,
                                               TextureSampling sampling)
{
    if (texture == nullptr)
        return;

    if (isDepth(kind))
        bindDepthTexture(*texture, handle.slot, sampling);
    else
        bindTexture(*texture, handle.slot, sampling);
}

void ShaderTextureBindVisitor::onStorageTexture(const char*,
                                                const StorageTextureLayout& layout,
                                                detail::TextureHandle& handle,
                                                const Texture* texture)
{
    assert(layout.access == TextureAccess::Read
           && "eacp: a render program cannot write a texture - a Write or "
              "ReadWrite storage texture belongs to a ComputeProgram");

    if (texture == nullptr || !detail::fitsStorageMember(*texture, layout)
        || detail::isCubeType(texture->kind()))
        return;

    constexpr auto unfiltered =
        TextureSampling {TextureFilter::Nearest, TextureAddressMode::Clamp};

    bindTexture(*texture, handle.slot, unfiltered);
}

void ShaderTextureBindVisitor::onSampler(const char* name,
                                         detail::SamplerHandle& handle,
                                         bool comparison,
                                         const Sampler& sampler)
{
    if (!detail::fitsSamplerMember(name, sampler, comparison))
        return;

    pass.setFragmentSampler(sampler, handle.slot);

    if (shader.vertexReadsTexture(handle.slot))
        pass.setVertexSampler(sampler, handle.slot);
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

void ShaderBufferBindVisitor::onIntInputBuffer(const char*,
                                               IntInputBuffer& handle,
                                               const BufferRange& range)
{
    if (!range.isValid())
        return;

    pass.setVertexStorageBuffer(range, handle.slot);
    pass.setFragmentStorageBuffer(range, handle.slot);
}

void ShaderBufferBindVisitor::onHalfInputBuffer(const char*,
                                                HalfInputBuffer& handle,
                                                const BufferRange& range)
{
    if (!range.isValid())
        return;

    pass.setVertexStorageBuffer(range, handle.slot);
    pass.setFragmentStorageBuffer(range, handle.slot);
}

void ShaderBufferBindVisitor::onHalfOutputBuffer(const char*,
                                                 HalfOutputBuffer&,
                                                 const BufferRange&)
{
    assert(false
           && "eacp: a render program cannot write a buffer - "
              "Uniform<HalfOutputBuffer> belongs to a ComputeProgram");
}

void ShaderBufferBindVisitor::onIntOutputBuffer(const char*,
                                                IntOutputBuffer&,
                                                const BufferRange&)
{
    assert(false
           && "eacp: a render program cannot write a buffer - "
              "Uniform<IntOutputBuffer> belongs to a ComputeProgram");
}

void ShaderBufferBindVisitor::onAtomicBuffer(const char*,
                                             AtomicBuffer&,
                                             const BufferRange&)
{
    assert(false
           && "eacp: a render program cannot write a buffer - "
              "Uniform<AtomicBuffer> belongs to a ComputeProgram");
}

void ShaderBufferBindVisitor::onIntAtomicBuffer(const char*,
                                                IntAtomicBuffer&,
                                                const BufferRange&)
{
    assert(false
           && "eacp: a render program cannot write a buffer - "
              "Uniform<IntAtomicBuffer> belongs to a ComputeProgram");
}

void ShaderBufferBindVisitor::onStructBuffer(const char*,
                                             BufferAccess access,
                                             detail::StructBufferHandle& handle,
                                             int (*)(ShaderGraph&),
                                             const BufferRange& range)
{
    assert(access == BufferAccess::Read
           && "eacp: a render program cannot write a buffer - "
              "Uniform<StructOutputBuffer<T>> belongs to a ComputeProgram");

    if (access != BufferAccess::Read || !range.isValid())
        return;

    pass.setVertexStorageBuffer(range, handle.slot);
    pass.setFragmentStorageBuffer(range, handle.slot);
}

void ShaderBufferBindVisitor::onWritableTexture(const char*,
                                                WritableTexture2D&,
                                                const Texture*)
{
    assert(false
           && "eacp: a render program cannot write a texture - "
              "Uniform<WritableTexture2D> belongs to a ComputeProgram");
}
} // namespace

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

bool ShaderProgram::fitsShaderFloat16(const Device& device) const
{
    return !device.isValid() || !graph().usesHalf()
           || device.supportsShaderFloat16();
}

bool ShaderProgram::fitsSampleRateShading(const Device& device) const
{
    return !device.isValid() || !graph().usesSampleRateShading()
           || device.supportsSampleRateShading();
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
    prepare(descriptor, OverrideValues {});
}

void ShaderProgram::prepare(RenderPipelineDescriptor descriptor,
                            const OverrideValues& overrides)
{
    specializations.clear();
    currentPipeline = nullptr;
    preparedDescriptor.reset();

    const auto fitsFloat16 = fitsShaderFloat16(Device::shared());

    if (!fitsFloat16)
        LOG("eacp: this shader computes in f16 and "
            "Device::supportsShaderFloat16 answers no, so no pipeline was "
            "built for it. Ask that query first, and where it answers no "
            "build the Float shader instead.");

    const auto fitsSampleRate = fitsSampleRateShading(Device::shared());

    if (!fitsSampleRate)
        LOG("eacp: this shader runs its fragment stage per sample (it reads "
            "sampleIndex() or a varying is Sample-interpolated) and "
            "Device::supportsSampleRateShading answers no, so no pipeline was "
            "built for it. Interpolate at the centre or the centroid instead.");

    if (!fitsFloat16 || !fitsSampleRate)
    {
        shaderLibrary.emplace(Device::shared(), ShaderSource {});
        useRefusedPipeline();
        return;
    }

    shaderLibrary.emplace(Device::shared(), generated.source);

    descriptor.library = &*shaderLibrary;
    descriptor.vertexLayout = generated.vertexLayout;
    preparedDescriptor = descriptor;

    specialize(overrides);
}

bool ShaderProgram::specialize(const OverrideValues& overrides)
{
    if (!preparedDescriptor.has_value())
        return false;

    const auto resolved = resolveOverrides(graph(), overrides);

    if (!resolved.isValid())
    {
        LOG("eacp: this shader refuses its override values (",
            resolved.error,
            "), so no pipeline was built for them.");
        useRefusedPipeline();
        return false;
    }

    for (const auto& specialization: specializations)
    {
        if (specialization->constants == resolved.specialized)
        {
            currentPipeline = &specialization->pipeline;
            return currentPipeline->isValid();
        }
    }

    auto descriptor = *preparedDescriptor;
    descriptor.constants = resolved.specialized;

    currentPipeline = &specializations.createNew(descriptor).pipeline;
    return currentPipeline->isValid();
}

void ShaderProgram::useRefusedPipeline()
{
    auto descriptor = RenderPipelineDescriptor {};
    refusedLibrary.emplace(Device::shared(), ShaderSource {});
    descriptor.library = &*refusedLibrary;
    descriptor.vertexLayout = generated.vertexLayout;
    refusedPipeline.emplace(Device::shared(), descriptor);
    currentPipeline = &*refusedPipeline;
}

const RenderPipeline& ShaderProgram::pipeline() const
{
    return *currentPipeline;
}

bool ShaderProgram::isPrecompiled() const
{
    return shaderLibrary.has_value() && shaderLibrary->isPrecompiled();
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
    return generated.vertexReadsUniforms();
}

bool ShaderProgram::fragmentReadsUniforms() const
{
    return generated.fragmentReadsUniforms();
}

const ShaderReflection& ShaderProgram::reflection() const
{
    return generated.reflection;
}

void ShaderProgram::bindTextures(RenderPass& pass)
{
    auto bindVisitor = ShaderTextureBindVisitor {pass, generated};
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

UInt ShaderProgram::varying(const UInt& vertexValue)
{
    return builder.varying(vertexValue);
}

Float ShaderProgram::varying(const Float& vertexValue,
                             const Interpolation& interpolation)
{
    return builder.varying(vertexValue, interpolation);
}

Float2 ShaderProgram::varying(const Float2& vertexValue,
                              const Interpolation& interpolation)
{
    return builder.varying(vertexValue, interpolation);
}

Float3 ShaderProgram::varying(const Float3& vertexValue,
                              const Interpolation& interpolation)
{
    return builder.varying(vertexValue, interpolation);
}

Float4 ShaderProgram::varying(const Float4& vertexValue,
                              const Interpolation& interpolation)
{
    return builder.varying(vertexValue, interpolation);
}

UInt ShaderProgram::varying(const UInt& vertexValue,
                            const Interpolation& interpolation)
{
    return builder.varying(vertexValue, interpolation);
}

Float4 ShaderProgram::fragmentCoordinate()
{
    return builder.fragmentCoordinate();
}

UInt ShaderProgram::vertexId()
{
    return builder.vertexId();
}

UInt ShaderProgram::instanceId()
{
    return builder.instanceId();
}

Bool ShaderProgram::frontFacing()
{
    return builder.frontFacing();
}

UInt ShaderProgram::sampleIndex()
{
    return builder.sampleIndex();
}

UInt ShaderProgram::sampleMask()
{
    return builder.sampleMask();
}

Float ShaderProgram::constant(float value)
{
    return builder.constant(value);
}

Half ShaderProgram::halfConstant(float value)
{
    return builder.halfConstant(value);
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

void ShaderProgram::breakLoopIf(const Bool& condition)
{
    builder.breakLoopIf(condition);
}

void ShaderProgram::returnEarly()
{
    builder.returnEarly();
}

void ShaderProgram::discard()
{
    builder.discard();
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

void ShaderProgram::setFragmentDepth(const Float& depth)
{
    builder.fragmentDepth(depth);
}

void ShaderProgram::setSampleMask(const UInt& mask)
{
    builder.fragmentSampleMask(mask);
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
