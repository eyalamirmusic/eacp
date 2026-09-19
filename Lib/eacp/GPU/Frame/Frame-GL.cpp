#include "Frame.h"

#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"
#include "../Texture/Texture.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>

namespace eacp::GPU
{
namespace
{
GLenum glDepthAttachmentPoint(const GLTextureData& data)
{
    return data.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
}

void glAttachDepth(const GLTextureData& data)
{
    if (!data.depth)
        return;

    const auto point = glDepthAttachmentPoint(data);

    if (data.depthRenderbuffer != 0)
    {
        glFramebufferRenderbuffer(
            GL_FRAMEBUFFER, point, GL_RENDERBUFFER, data.depthRenderbuffer);
        return;
    }

    glFramebufferTexture2D(
        GL_FRAMEBUFFER, point, GL_TEXTURE_2D, data.depthTexture, 0);
}

// The framebuffer a pass draws into: the multisampled companion where the
// target has one, and the target's own otherwise. The attachments are made
// every time rather than cached, since a read-back may have built the colour
// one on its own and there is no second question to ask about the answer.
GLuint glPassFramebuffer(GLTextureData& data)
{
    const auto multisampled = data.sampleCount > 1 && data.msaaColor != 0;
    auto& framebuffer = multisampled ? data.msaaFramebuffer : data.framebuffer;

    if (framebuffer == 0)
        glGenFramebuffers(1, &framebuffer);

    if (framebuffer == 0)
        return 0;

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    if (multisampled)
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                  GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER,
                                  data.msaaColor);
    else
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               data.texture,
                               0);

    glAttachDepth(data);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        return framebuffer;

    LOG("OpenGL: the render target's framebuffer came back incomplete, so the "
        "pass records nothing rather than drawing into an undefined one");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    framebuffer = 0;

    return 0;
}

struct GLFrameBackend final : FrameBackend
{
    // Stage 3's: a drawable is an EGLSurface over the view's own surface, and
    // until there is one a frame built on a drawable has no target at all -
    // isValid() is false and beginPass answers null, which is what the portable
    // half turns into a RenderPass over no encoder.
    GLFrameBackend(Device& deviceToUse, void*)
        : device(&deviceToUse)
        , context(getGLContext(deviceToUse))
    {
        open();
    }

    GLFrameBackend(Device& deviceToUse, const OffscreenTarget& offscreen)
        : device(&deviceToUse)
        , context(getGLContext(deviceToUse))
        , target(static_cast<GLTextureData*>(offscreen.colorTexture))
    {
        open();

        if (target == nullptr || !target->isValid() || !target->renderTarget)
        {
            target = nullptr;
            return;
        }

        frame.width = target->width;
        frame.height = target->height;
    }

    void open()
    {
        if (!context.isValid())
            return;

        context.makeCurrent();
        frame.ringAlignment =
            context.getCapabilities().uniformBufferOffsetAlignment;
    }

    ~GLFrameBackend() override
    {
        if (!context.isValid())
            return;

        context.makeCurrent();

        device->frameTimer().endFrame(nullptr);
        device->frameTimer().noteSubmitted(1);

        frame.releaseRing();

        // Everything recorded is in the context's own stream, which is in
        // order, so a read of what this frame drew needs no wait of its own -
        // the flush is what starts it moving rather than what it waits for.
        glFlush();
    }

    // The frame's own start, before anything else is recorded, which is what
    // makes the total mean the frame.
    void beginTiming() override
    {
        if (context.isValid())
            device->frameTimer().beginRecording(nullptr);
    }

    void timePass(GLRenderEncoder& encoder, std::string_view label)
    {
        auto& timer = device->frameTimer();
        const auto pass = timer.beginPass(label);

        if (pass < 0)
            return;

        auto* slot = static_cast<GLTimestampSlot*>(timer.nativeSamples());

        if (slot == nullptr)
            return;

        slot->writeTimestamp(pass * 2);

        encoder.timing = slot;
        encoder.endQuery = pass * 2 + 1;
    }

    std::unique_ptr<RenderPassBackend>
        beginPassOn(GLTextureData& data, const RenderPassDescriptor& descriptor)
    {
        if (!context.isValid() || !data.isValid() || !data.renderTarget)
            return nullptr;

        context.makeCurrent();

        const auto framebuffer = glPassFramebuffer(data);

        if (framebuffer == 0)
            return nullptr;

        auto* encoder = new GLRenderEncoder {};

        encoder->device = device;
        encoder->frame = &frame;
        encoder->target = &data;
        encoder->framebuffer = framebuffer;
        encoder->width = data.width;
        encoder->height = data.height;

        // A texture's row 0 is its own y = 0, and GL rasterizes clip +1 into the
        // highest row, so the sign turns eacp's top row into texel row 0 (D7).
        encoder->clipYSign = -1.f;

        timePass(*encoder, descriptor.label);
        applyLoadActions(data, descriptor);

        return makeGLRenderPass(encoder);
    }

    // Every clear is masked by the write masks and the scissor, so the state
    // they start from is set here rather than assumed - and it is the state the
    // pass records as last applied, so the first setPipeline diffs against what
    // is really there.
    void applyLoadActions(const GLTextureData& data,
                          const RenderPassDescriptor& descriptor)
    {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(0xff);

        glViewport(0, 0, (GLsizei) data.width, (GLsizei) data.height);

        if (descriptor.clear)
        {
            const auto& color = descriptor.clearColor;
            const GLfloat values[] = {color.r, color.g, color.b, color.a};

            glClearBufferfv(GL_COLOR, 0, values);
        }

        if (!data.depth || descriptor.depthAction == DepthAction::Resume)
            return;

        if (data.stencil)
        {
            glClearBufferfi(
                GL_DEPTH_STENCIL, 0, 1.f, (GLint) descriptor.clearStencil);
            return;
        }

        const GLfloat far = 1.f;
        glClearBufferfv(GL_DEPTH, 0, &far);
    }

    bool isValid() const override
    {
        return target != nullptr && target->isValid() && target->renderTarget;
    }

    Graphics::Point pixelSize() const override
    {
        return {static_cast<float>(frame.width), static_cast<float>(frame.height)};
    }

    // glFlush starts the work; the fence is what makes waiting for it possible
    // at all, and a read recorded after this one is already behind it.
    void flush() override
    {
        if (!context.isValid())
            return;

        context.makeCurrent();

        auto* fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

        glFlush();

        if (fence == nullptr)
            return;

        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(fence);
    }

    std::unique_ptr<RenderPassBackend>
        beginPass(const RenderPassDescriptor& descriptor) override
    {
        if (target == nullptr)
            return nullptr;

        return beginPassOn(*target, descriptor);
    }

    std::unique_ptr<RenderPassBackend>
        beginPass(const Texture& passTarget,
                  const RenderPassDescriptor& descriptor) override
    {
        auto* data = static_cast<GLTextureData*>(passTarget.nativeTexture());

        if (data == nullptr || !passTarget.isRenderTarget())
            return nullptr;

        return beginPassOn(*data, descriptor);
    }

    // Stage 6's, and a null pass is what the portable half drops every dispatch
    // under (D9).
    std::unique_ptr<ComputePassBackend> beginCompute(std::string_view,
                                                     DispatchOrder) override
    {
        return nullptr;
    }

    Device* device = nullptr;
    GLContext& context;

    GLTextureData* target = nullptr;
    GLFrameData frame;
};
} // namespace

// Out of line, beside the frame that owns the ring: GLTypes.h is the structs
// the -GL.cpp files cast to and nothing else.
GLFrameData::Range GLFrameData::writeUniforms(const void* data,
                                              int bytes,
                                              int leastBytes)
{
    const auto size = (GLsizeiptr) std::max(bytes, leastBytes);

    if (data == nullptr || bytes <= 0 || size <= 0)
        return {};

    if (uniformRing == 0)
        glGenBuffers(1, &uniformRing);

    if (uniformRing == 0)
        return {};

    const auto alignment = (GLintptr) (ringAlignment > 0 ? ringAlignment : 4);
    auto offset = (ringCursor + alignment - 1) / alignment * alignment;

    glBindBuffer(GL_UNIFORM_BUFFER, uniformRing);

    // Grown by starting again, which costs nothing a draw can see: GL runs its
    // commands in order, so every draw already recorded has read what it was
    // given before this respecification reaches it.
    if (offset + size > ringCapacity)
    {
        auto capacity = ringCapacity > 0 ? ringCapacity : (GLsizeiptr) 65536;

        while (capacity < size)
            capacity *= 2;

        glBufferData(GL_UNIFORM_BUFFER, capacity, nullptr, GL_STREAM_DRAW);

        ringCapacity = capacity;
        offset = 0;
    }

    glBufferSubData(GL_UNIFORM_BUFFER, offset, (GLsizeiptr) bytes, data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    ringCursor = offset + size;

    return {uniformRing, offset, size};
}

void GLFrameData::releaseRing()
{
    if (uniformRing == 0)
        return;

    glDeleteBuffers(1, &uniformRing);

    uniformRing = 0;
    ringCapacity = 0;
    ringCursor = 0;
}

std::unique_ptr<FrameBackend> makeGLFrame(Device& device, void* drawable)
{
    return std::make_unique<GLFrameBackend>(device, drawable);
}

std::unique_ptr<FrameBackend> makeGLFrame(Device& device,
                                          const OffscreenTarget& target)
{
    return std::make_unique<GLFrameBackend>(device, target);
}
} // namespace eacp::GPU
