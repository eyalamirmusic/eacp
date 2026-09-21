#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Codegen/ShaderBindings.h"
#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace eacp::GPU
{
namespace
{
// The eight the render stage has, which is what both the texture and the
// storage-buffer slot spaces are counted in.
constexpr int glMaxBufferSlots = 8;
constexpr int glMaxVertexSlots = 8;

// The unit left active whenever nothing is being bound, one past the slots a
// pass can bind. Everything else that binds a texture - an upload, a read-back -
// binds it to whichever unit is active, and this is what keeps such a bind from
// taking a sampled texture out from under an open pass.
constexpr int glScratchTextureUnit = maxTextureSlots;

GLenum glTopologyFor(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::Triangles:
            return GL_TRIANGLES;
        case PrimitiveTopology::TriangleStrip:
            return GL_TRIANGLE_STRIP;
        case PrimitiveTopology::Lines:
            return GL_LINES;
        case PrimitiveTopology::LineStrip:
            return GL_LINE_STRIP;
        case PrimitiveTopology::Points:
            return GL_POINTS;
    }

    return GL_TRIANGLES;
}

// UNORM/SNORM, not the integer forms: the shader reads these as 0..1 and -1..1,
// which is what the normalized flag says.
struct GLVertexFormat
{
    GLint components = 0;
    GLenum type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
};

GLVertexFormat glVertexFormatFor(VertexFormat format)
{
    switch (format)
    {
        case VertexFormat::Float:
            return {1, GL_FLOAT, GL_FALSE};
        case VertexFormat::Float2:
            return {2, GL_FLOAT, GL_FALSE};
        case VertexFormat::Float3:
            return {3, GL_FLOAT, GL_FALSE};
        case VertexFormat::Float4:
            return {4, GL_FLOAT, GL_FALSE};
        case VertexFormat::UByte4Norm:
            return {4, GL_UNSIGNED_BYTE, GL_TRUE};
        case VertexFormat::Half2:
            return {2, GL_HALF_FLOAT, GL_FALSE};
        case VertexFormat::Half4:
            return {4, GL_HALF_FLOAT, GL_FALSE};
        case VertexFormat::Short2Norm:
            return {2, GL_SHORT, GL_TRUE};
        case VertexFormat::Short4Norm:
            return {4, GL_SHORT, GL_TRUE};
    }

    return {3, GL_FLOAT, GL_FALSE};
}

GLenum glCompareFor(CompareFunction compare)
{
    switch (compare)
    {
        case CompareFunction::Never:
            return GL_NEVER;
        case CompareFunction::Less:
            return GL_LESS;
        case CompareFunction::LessEqual:
            return GL_LEQUAL;
        case CompareFunction::Equal:
            return GL_EQUAL;
        case CompareFunction::NotEqual:
            return GL_NOTEQUAL;
        case CompareFunction::GreaterEqual:
            return GL_GEQUAL;
        case CompareFunction::Greater:
            return GL_GREATER;
        case CompareFunction::Always:
            return GL_ALWAYS;
    }

    return GL_LEQUAL;
}

GLenum glStencilOpFor(StencilOp op)
{
    switch (op)
    {
        case StencilOp::Keep:
            return GL_KEEP;
        case StencilOp::Zero:
            return GL_ZERO;
        case StencilOp::Replace:
            return GL_REPLACE;
        case StencilOp::IncrementClamp:
            return GL_INCR;
        case StencilOp::DecrementClamp:
            return GL_DECR;
        case StencilOp::Invert:
            return GL_INVERT;
        case StencilOp::IncrementWrap:
            return GL_INCR_WRAP;
        case StencilOp::DecrementWrap:
            return GL_DECR_WRAP;
    }

    return GL_KEEP;
}

GLenum glBlendFactorFor(BlendFactor factor)
{
    switch (factor)
    {
        case BlendFactor::Zero:
            return GL_ZERO;
        case BlendFactor::One:
            return GL_ONE;
        case BlendFactor::SourceColor:
            return GL_SRC_COLOR;
        case BlendFactor::OneMinusSourceColor:
            return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SourceAlpha:
            return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSourceAlpha:
            return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DestinationColor:
            return GL_DST_COLOR;
        case BlendFactor::OneMinusDestinationColor:
            return GL_ONE_MINUS_DST_COLOR;
        case BlendFactor::DestinationAlpha:
            return GL_DST_ALPHA;
        case BlendFactor::OneMinusDestinationAlpha:
            return GL_ONE_MINUS_DST_ALPHA;
        case BlendFactor::SourceAlphaSaturated:
            return GL_SRC_ALPHA_SATURATE;
    }

    return GL_ONE;
}

GLenum glBlendOperationFor(BlendOperation operation)
{
    switch (operation)
    {
        case BlendOperation::Add:
            return GL_FUNC_ADD;
        case BlendOperation::Subtract:
            return GL_FUNC_SUBTRACT;
        case BlendOperation::ReverseSubtract:
            return GL_FUNC_REVERSE_SUBTRACT;
        case BlendOperation::Min:
            return GL_MIN;
        case BlendOperation::Max:
            return GL_MAX;
    }

    return GL_FUNC_ADD;
}

bool glBlendIsEqual(const BlendState& a, const BlendState& b)
{
    if (a.enabled != b.enabled)
        return false;

    if (!a.enabled)
        return true;

    return a.sourceColor == b.sourceColor && a.destinationColor == b.destinationColor
           && a.colorOperation == b.colorOperation && a.sourceAlpha == b.sourceAlpha
           && a.destinationAlpha == b.destinationAlpha
           && a.alphaOperation == b.alphaOperation;
}

bool glMaskIsEqual(const ColorWriteMask& a, const ColorWriteMask& b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue
           && a.alpha == b.alpha;
}

bool glFaceIsEqual(const StencilFace& a, const StencilFace& b)
{
    return a.compare == b.compare && a.stencilFail == b.stencilFail
           && a.depthFail == b.depthFail && a.pass == b.pass;
}

// The depth range under both spellings: the float one is core only from GL 4.1,
// and ES has no other.
void glSetDepthRange(float nearDepth, float farDepth)
{
    if (glad_glDepthRange != nullptr)
        glDepthRange((GLdouble) nearDepth, (GLdouble) farDepth);
    else
        glDepthRangef(nearDepth, farDepth);
}

struct GLBoundBuffer
{
    GLuint buffer = 0;
    std::int64_t offset = 0;
};

struct GLRenderPassBackend final : RenderPassBackend
{
    explicit GLRenderPassBackend(GLRenderEncoder* encoderToUse)
        : encoder(encoderToUse)
        , context(getGLContext(*encoderToUse->device))
    {
    }

    ~GLRenderPassBackend() override { end(); }

    int targetWidth() const override
    {
        return encoder != nullptr ? encoder->width : 0;
    }

    int targetHeight() const override
    {
        return encoder != nullptr ? encoder->height : 0;
    }

    // A rect arrives in target pixels counted from the top of the picture, and
    // GL counts framebuffer rows from the bottom. On a texture those are the
    // same rows - its row 0 is its own y = 0, which is what the y sign turns
    // the picture the right way up for - and on a drawable they are opposite.
    int windowY(int top, int height) const
    {
        if (encoder->clipYSign < 0.f)
            return top;

        return encoder->height - (top + height);
    }

    void setScissorRect(const Graphics::Rect& rect) override
    {
        if (encoder == nullptr || encoder->width <= 0 || encoder->height <= 0)
            return;

        // Outward: rounding an edge inward would shave a column of coverage off
        // it.
        const auto left = std::clamp((int) std::floor(rect.x), 0, encoder->width);
        const auto top = std::clamp((int) std::floor(rect.y), 0, encoder->height);
        const auto right =
            std::clamp((int) std::ceil(rect.x + rect.w), left, encoder->width);
        const auto bottom =
            std::clamp((int) std::ceil(rect.y + rect.h), top, encoder->height);

        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint) left,
                  (GLint) windowY(top, bottom - top),
                  (GLsizei) (right - left),
                  (GLsizei) (bottom - top));

        scissorOn = true;
    }

    void clearScissorRect() override
    {
        if (encoder == nullptr)
            return;

        glDisable(GL_SCISSOR_TEST);
        scissorOn = false;
    }

    void setViewport(const Graphics::Rect& rect,
                     float nearDepth,
                     float farDepth) override
    {
        if (encoder == nullptr || encoder->width <= 0 || encoder->height <= 0)
            return;

        if (rect.w <= 0.f || rect.h <= 0.f || rect.x < 0.f || rect.y < 0.f
            || rect.x + rect.w > (float) encoder->width
            || rect.y + rect.h > (float) encoder->height)
            return;

        const auto height = (int) rect.h;

        glViewport((GLint) rect.x,
                   (GLint) windowY((int) rect.y, height),
                   (GLsizei) rect.w,
                   (GLsizei) height);

        glSetDepthRange(nearDepth, farDepth);
    }

    void clearViewport() override
    {
        if (encoder == nullptr)
            return;

        glViewport(0, 0, (GLsizei) encoder->width, (GLsizei) encoder->height);
        glSetDepthRange(0.f, 1.f);
    }

    void setPipeline(const RenderPipeline& toBind) override
    {
        if (encoder == nullptr)
            return;

        auto* state = static_cast<GLRenderPipelineData*>(toBind.nativeState());

        pipeline = nullptr;

        if (state == nullptr || !state->isValid())
            return;

        if (!samplesMatch(*state))
        {
            LOG("OpenGL: the pipeline was built for ",
                state->state.sampleCount,
                " samples and the target carries ",
                targetSamples(),
                ", so the draw is refused rather than drawn into the wrong "
                "attachment");
            return;
        }

        pipeline = state;

        glUseProgram(state->program);

        if (state->clipYSignLocation >= 0)
            glUniform1f(state->clipYSignLocation, encoder->clipYSign);

        applyState(state->state);

        layoutDirty = true;
        uniformsDirty = true;
    }

    int targetSamples() const { return encoder->samples; }

    bool samplesMatch(const GLRenderPipelineData& state) const
    {
        return state.state.sampleCount == targetSamples();
    }

    // Diffed against what the pass last applied, so a pipeline switch costs only
    // the calls that change - and applied whole the first time, since what the
    // pass before it left is not this pass's to assume (D6).
    void applyState(const GLPipelineState& state)
    {
        const auto force = !stateApplied;

        if (force || !glBlendIsEqual(state.blend, applied.blend))
            applyBlend(state.blend);

        if (force || !glMaskIsEqual(state.colorWriteMask, applied.colorWriteMask))
        {
            const auto& mask = state.colorWriteMask;

            glColorMask(mask.red, mask.green, mask.blue, mask.alpha);
        }

        if (force || state.depth != applied.depth
            || state.depthCompare != applied.depthCompare)
        {
            if (state.depth)
            {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(glCompareFor(state.depthCompare));
            }
            else
            {
                glDisable(GL_DEPTH_TEST);
            }
        }

        if (force || state.depthWrite != applied.depthWrite)
            glDepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);

        applyStencil(state, force);

        if (force || state.cullMode != applied.cullMode)
        {
            if (state.cullMode == CullMode::None)
            {
                glDisable(GL_CULL_FACE);
            }
            else
            {
                glEnable(GL_CULL_FACE);
                glCullFace(state.cullMode == CullMode::Front ? GL_FRONT : GL_BACK);
            }
        }

        if (force || state.frontFace != applied.frontFace)
            glFrontFace(frontFaceFor(state.frontFace));

        applied = state;
        stateApplied = true;
    }

    void applyBlend(const BlendState& blend)
    {
        if (!blend.enabled)
        {
            glDisable(GL_BLEND);
            return;
        }

        glEnable(GL_BLEND);
        glBlendFuncSeparate(glBlendFactorFor(blend.sourceColor),
                            glBlendFactorFor(blend.destinationColor),
                            glBlendFactorFor(blend.sourceAlpha),
                            glBlendFactorFor(blend.destinationAlpha));
        glBlendEquationSeparate(glBlendOperationFor(blend.colorOperation),
                                glBlendOperationFor(blend.alphaOperation));
    }

    void applyStencil(const GLPipelineState& state, bool force)
    {
        if (force || state.stencil != applied.stencil)
        {
            if (state.stencil)
                glEnable(GL_STENCIL_TEST);
            else
                glDisable(GL_STENCIL_TEST);
        }

        if (force || state.stencilWriteMask != applied.stencilWriteMask)
            glStencilMask(state.stencilWriteMask);

        const auto facesChanged =
            force || !glFaceIsEqual(state.stencilFront, applied.stencilFront)
            || !glFaceIsEqual(state.stencilBack, applied.stencilBack)
            || state.stencilReadMask != applied.stencilReadMask;

        if (!facesChanged)
            return;

        applyStencilFace(GL_FRONT, state.stencilFront, state.stencilReadMask);
        applyStencilFace(GL_BACK, state.stencilBack, state.stencilReadMask);
    }

    void applyStencilFace(GLenum face,
                          const StencilFace& description,
                          unsigned char readMask)
    {
        glStencilFuncSeparate(face,
                              glCompareFor(description.compare),
                              (GLint) stencilReference,
                              readMask);
        glStencilOpSeparate(face,
                            glStencilOpFor(description.stencilFail),
                            glStencilOpFor(description.depthFail),
                            glStencilOpFor(description.pass));
    }

    // eacp's convention is glTF's - counter-clockwise in the clip space the
    // vertex stage writes, y up - and the wrapper's multiply has reversed the
    // winding of every triangle on a texture target, so the answer is reversed
    // with it.
    GLenum frontFaceFor(Winding winding) const
    {
        const auto counterClockwise =
            (winding == Winding::CounterClockwise) == (encoder->clipYSign > 0.f);

        return counterClockwise ? GL_CCW : GL_CW;
    }

    void setStencilReference(unsigned int value) override
    {
        if (encoder == nullptr || stencilReference == value)
            return;

        stencilReference = value;

        if (!stateApplied)
            return;

        applyStencilFace(GL_FRONT, applied.stencilFront, applied.stencilReadMask);
        applyStencilFace(GL_BACK, applied.stencilBack, applied.stencilReadMask);
    }

    void setVertexBuffer(const BufferRange& range, int index) override
    {
        if (encoder == nullptr || index < 0 || index >= glMaxVertexSlots)
            return;

        auto* data = bufferOf(range);

        if (data == nullptr)
            return;

        vertexBuffers[index] = {data->buffer, range.offset};
        layoutDirty = true;
    }

    GLBufferData* bufferOf(const BufferRange& range) const
    {
        if (range.buffer == nullptr)
            return nullptr;

        auto* data = static_cast<GLBufferData*>(range.buffer->nativeBuffer());

        if (data == nullptr || !data->isValid() || range.offset < 0
            || range.offset >= data->size)
            return nullptr;

        return data;
    }

    void setFragmentTexture(const Texture& texture,
                            int slot,
                            TextureSampling sampling) override
    {
        auto* data = static_cast<GLTextureData*>(texture.nativeTexture());

        if (data == nullptr || !data->isValid())
            return;

        bindTexture(data->target, data->texture, slot, sampling);
    }

    void setFragmentDepthTexture(const Texture& renderTarget,
                                 int slot,
                                 TextureSampling sampling) override
    {
        auto* data = static_cast<GLTextureData*>(renderTarget.nativeTexture());

        if (data == nullptr || !data->sampleableDepth)
            return;

        const auto texture = data->resolvedDepthTexture != 0
                                 ? data->resolvedDepthTexture
                                 : data->depthTexture;

        if (texture == 0)
            return;

        bindTexture(GL_TEXTURE_2D, texture, slot, sampling);
    }

    void bindTexture(GLenum target,
                     GLuint texture,
                     int slot,
                     TextureSampling sampling)
    {
        if (encoder == nullptr || slot < 0 || slot >= maxTextureSlots)
            return;

        const auto unit = glTextureUnitFor(vulkanTextureBinding(slot));
        const auto sampler =
            (GLuint) (std::uintptr_t) encoder->device->nativeSampler(sampling);

        glActiveTexture((GLenum) (GL_TEXTURE0 + unit));
        glBindTexture(target, texture);
        glBindSampler((GLuint) unit, sampler);

        glActiveTexture((GLenum) (GL_TEXTURE0 + glScratchTextureUnit));
    }

    void setStorageBuffer(const BufferRange& range, int slot) override
    {
        if (encoder == nullptr || slot < 0 || slot >= glMaxBufferSlots)
            return;

        if (!context.getCapabilities().storageBuffers)
        {
            LOG("OpenGL: this context has no shader storage buffers, so the "
                "bind is refused rather than left pointing at nothing");
            return;
        }

        auto* data = bufferOf(range);

        if (data == nullptr)
            return;

        const auto alignment = encoder->device->storageBufferOffsetAlignment();

        if (alignment > 0 && range.offset % alignment != 0)
            return;

        const auto point = glStorageBindingPoint(vulkanBufferBinding(slot));
        const auto bytes = data->size - range.offset;

        glBindBufferRange(GL_SHADER_STORAGE_BUFFER,
                          (GLuint) point,
                          data->buffer,
                          (GLintptr) range.offset,
                          (GLsizeiptr) bytes);
    }

    // Kept rather than written, so the block is sized by the pipeline the draw
    // ends up on rather than by whichever one was bound when this was called.
    void setBytes(const void* data, int bytes, int slot) override
    {
        if (encoder == nullptr || data == nullptr || bytes <= 0 || slot < 0)
            return;

        uniformBytes.resize((std::size_t) bytes);
        std::memcpy(uniformBytes.data(), data, (std::size_t) bytes);

        uniformsDirty = true;
    }

    bool canRecord() const { return encoder != nullptr && pipeline != nullptr; }

    void bindUniforms()
    {
        if (!uniformsDirty || uniformBytes.empty()
            || pipeline->uniformBlocks.empty())
            return;

        uniformsDirty = false;

        const auto range = encoder->frame->writeUniforms(uniformBytes.data(),
                                                         (int) uniformBytes.size(),
                                                         pipeline->uniformBlockSize);

        if (!range.isValid())
            return;

        for (const auto& block: pipeline->uniformBlocks)
            glBindBufferRange(GL_UNIFORM_BUFFER,
                              (GLuint) block.binding,
                              range.buffer,
                              range.offset,
                              range.bytes);
    }

    int strideForSlot(int slot) const
    {
        const auto& layout = pipeline->vertexLayout;

        if (slot >= 0 && slot < layout.buffers.size())
            return layout.buffers[slot].stride;

        return layout.stride;
    }

    bool isPerInstance(int slot) const
    {
        const auto& layout = pipeline->vertexLayout;

        if (slot >= 0 && slot < layout.buffers.size())
            return layout.buffers[slot].stepRate == StepRate::PerInstance;

        return false;
    }

    // baseVertex and firstInstance are the attribute pointers' rather than the
    // draw's: GL puts each behind an extension the floor does not have - base
    // instance is 4.2 and never ES, base vertex never ES 3.0 - while the whole
    // of what either means, no shader in the tree reading a vertex or instance
    // id, is where a slot starts reading. So they are folded in here and every
    // context draws through the one call (D6 as built).
    bool applyVertexLayout(int baseVertex, int firstInstance)
    {
        if (!layoutDirty && baseVertex == appliedBaseVertex
            && firstInstance == appliedFirstInstance)
            return true;

        glBindVertexArray(context.getVertexArray());

        const auto& attributes = pipeline->vertexLayout.attributes;

        for (auto i = 0; i < attributes.size(); ++i)
            if (!applyAttribute(i, attributes[i], baseVertex, firstInstance))
                return false;

        for (auto i = attributes.size(); i < context.getEnabledVertexAttributes();
             ++i)
            glDisableVertexAttribArray((GLuint) i);

        context.setEnabledVertexAttributes(attributes.size());

        layoutDirty = false;
        appliedBaseVertex = baseVertex;
        appliedFirstInstance = firstInstance;

        return true;
    }

    bool applyAttribute(int index,
                        const VertexAttribute& attribute,
                        int baseVertex,
                        int firstInstance)
    {
        const auto slot = attribute.bufferIndex;
        const auto bound = slot >= 0 && slot < glMaxVertexSlots ? vertexBuffers[slot]
                                                                : GLBoundBuffer {};

        if (bound.buffer == 0)
        {
            glDisableVertexAttribArray((GLuint) index);
            return true;
        }

        const auto instanced = isPerInstance(slot);
        const auto stride = strideForSlot(slot);
        const auto first = instanced ? firstInstance : baseVertex;
        const auto offset =
            bound.offset + attribute.offset + (std::int64_t) first * stride;

        if (offset < 0)
            return false;

        const auto format = glVertexFormatFor(attribute.format);

        glBindBuffer(GL_ARRAY_BUFFER, bound.buffer);
        glEnableVertexAttribArray((GLuint) index);
        glVertexAttribPointer((GLuint) index,
                              format.components,
                              format.type,
                              format.normalized,
                              (GLsizei) stride,
                              reinterpret_cast<const void*>(offset));
        glVertexAttribDivisor((GLuint) index, instanced ? 1 : 0);

        return true;
    }

    void drawInstanced(int vertexCount,
                       int instanceCount,
                       int firstVertex,
                       int firstInstance) override
    {
        if (!canRecord() || vertexCount <= 0 || instanceCount <= 0)
            return;

        if (!applyVertexLayout(0, firstInstance))
            return;

        bindUniforms();

        glDrawArraysInstanced(glTopologyFor(pipeline->state.topology),
                              (GLint) firstVertex,
                              (GLsizei) vertexCount,
                              (GLsizei) instanceCount);
    }

    void drawIndexedInstanced(const BufferRange& indices,
                              int indexCount,
                              int instanceCount,
                              IndexFormat format,
                              int firstIndex,
                              int firstInstance,
                              int baseVertex) override
    {
        if (!canRecord() || indexCount <= 0 || instanceCount <= 0)
            return;

        auto* data = bufferOf(indices);

        if (data == nullptr)
            return;

        if (!applyVertexLayout(baseVertex, firstInstance))
            return;

        bindUniforms();

        const auto indexBytes = format == IndexFormat::UInt16 ? 2 : 4;
        const auto offset = indices.offset + (std::int64_t) firstIndex * indexBytes;

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, data->buffer);
        glDrawElementsInstanced(glTopologyFor(pipeline->state.topology),
                                (GLsizei) indexCount,
                                format == IndexFormat::UInt16 ? GL_UNSIGNED_SHORT
                                                              : GL_UNSIGNED_INT,
                                reinterpret_cast<const void*>(offset),
                                (GLsizei) instanceCount);
    }

    void end() override
    {
        if (encoder == nullptr)
            return;

        if (encoder->timing != nullptr && encoder->endQuery >= 0)
            encoder->timing->writeTimestamp(encoder->endQuery);

        resolveMultisampling();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glUseProgram(0);

        glDrainErrors("render pass");

        encoder.reset();
    }

    // What every other backend does at the end of a multisampled pass: the
    // texture the app holds is the resolved picture, whoever samples or reads
    // it. The blit is masked by the write masks and the scissor, so both are put
    // back first.
    void resolveMultisampling()
    {
        auto* data = encoder->target;

        if (data == nullptr)
        {
            resolveIntoDefaultFramebuffer();
            return;
        }

        if (data->sampleCount <= 1 || data->msaaFramebuffer == 0)
            return;

        if (scissorOn)
            glDisable(GL_SCISSOR_TEST);

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        const auto color = resolveFramebuffer(*data);

        if (color != 0)
            blitInto(color, GL_COLOR_BUFFER_BIT);

        if (!data->sampleableDepth)
            return;

        const auto depth = resolvedDepthFramebuffer(*data);

        if (depth != 0)
            blitInto(depth,
                     data->stencil ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT
                                   : GL_DEPTH_BUFFER_BIT);
    }

    // A view whose surface carries what it asked for draws into the default
    // framebuffer and has nothing to resolve; one that needed a companion - a
    // depth plane, more than one sample, or both - resolves out of it here,
    // which is the same blit a multisampled texture takes.
    void resolveIntoDefaultFramebuffer()
    {
        if (!encoder->resolveToDefault || encoder->framebuffer == 0)
            return;

        if (scissorOn)
            glDisable(GL_SCISSOR_TEST);

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        blitInto(0, GL_COLOR_BUFFER_BIT);
    }

    void blitInto(GLuint destination, GLbitfield planes)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, encoder->framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination);

        glBlitFramebuffer(0,
                          0,
                          (GLint) encoder->width,
                          (GLint) encoder->height,
                          0,
                          0,
                          (GLint) encoder->width,
                          (GLint) encoder->height,
                          planes,
                          GL_NEAREST);
    }

    GLuint resolveFramebuffer(GLTextureData& data) const
    {
        if (data.framebuffer == 0)
            glGenFramebuffers(1, &data.framebuffer);

        glBindFramebuffer(GL_FRAMEBUFFER, data.framebuffer);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, data.texture, 0);

        return data.framebuffer;
    }

    GLuint resolvedDepthFramebuffer(GLTextureData& data) const
    {
        if (data.resolvedDepthTexture == 0)
            return 0;

        if (data.resolvedDepthFramebuffer == 0)
            glGenFramebuffers(1, &data.resolvedDepthFramebuffer);

        glBindFramebuffer(GL_FRAMEBUFFER, data.resolvedDepthFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               data.stencil ? GL_DEPTH_STENCIL_ATTACHMENT
                                            : GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D,
                               data.resolvedDepthTexture,
                               0);

        return data.resolvedDepthFramebuffer;
    }

    std::unique_ptr<GLRenderEncoder> encoder;
    GLContext& context;

    GLRenderPipelineData* pipeline = nullptr;

    // What the pass last applied, and whether it has applied anything at all:
    // the first setPipeline writes the whole struct, since the state the pass
    // before it left is not this pass's to assume.
    GLPipelineState applied;
    bool stateApplied = false;
    bool scissorOn = false;

    unsigned int stencilReference = 0;

    Array<GLBoundBuffer, glMaxVertexSlots> vertexBuffers {};

    std::vector<std::byte> uniformBytes;
    bool uniformsDirty = false;

    bool layoutDirty = true;
    int appliedBaseVertex = 0;
    int appliedFirstInstance = 0;
};
} // namespace

std::unique_ptr<RenderPassBackend> makeGLRenderPass(GLRenderEncoder* encoder)
{
    return std::make_unique<GLRenderPassBackend>(encoder);
}
} // namespace eacp::GPU
