#include "RenderPipeline.h"

#include "../Codegen/ShaderBindings.h"
#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"
#include "../Shader/ShaderLibrary.h"

#include <eacp/Core/Utils/Logging.h>

#include <string>

namespace eacp::GPU
{
namespace
{
std::string glProgramLog(GLuint program)
{
    auto length = GLint {0};
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);

    if (length <= 1)
        return {};

    auto log = std::string((std::size_t) length, '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());

    while (!log.empty() && log.back() == '\0')
        log.pop_back();

    return log;
}

// Whether a pass could attach what this pipeline says it writes into. The
// Vulkan backend refuses a pipeline whose samples it cannot resolve for the
// same reason: a format the context cannot render is a draw that would be
// discovered at the framebuffer-completeness check instead.
bool glColorFormatIsRenderable(PixelFormat format, const GLCapabilities& caps)
{
    switch (format)
    {
        case PixelFormat::BGRA8Unorm:
        case PixelFormat::RGBA8Unorm:
            return true;

        case PixelFormat::RGBA16Float:
            return caps.halfFloatRenderTargets;

        case PixelFormat::RGBA32Float:
        case PixelFormat::R32Float:
            return caps.floatRenderTargets;
    }

    return false;
}

struct GLRenderPipelineBackend final : RenderPipelineBackend
{
    GLRenderPipelineBackend(Device& device,
                            const RenderPipelineDescriptor& descriptor)
        : context(getGLContext(device))
        , topologyKind(descriptor.topology)
        , cull(descriptor.cullMode)
        , winding(descriptor.frontFace)
    {
        auto& state = pipeline.state;

        state.topology = descriptor.topology;
        state.blend = descriptor.blend ? *descriptor.blend
                                       : blendStateFor(descriptor.blendMode);
        state.colorWriteMask = descriptor.colorWriteMask;
        state.depth = descriptor.depth;
        state.depthCompare = descriptor.depthCompare;
        state.depthWrite = descriptor.depthWrite;
        state.stencil = descriptor.stencil;
        state.stencilFront = descriptor.stencilFront;
        state.stencilBack = descriptor.stencilBack;
        state.stencilReadMask = descriptor.stencilReadMask;
        state.stencilWriteMask = descriptor.stencilWriteMask;
        state.cullMode = descriptor.cullMode;
        state.frontFace = descriptor.frontFace;
        state.sampleCount = descriptor.sampleCount > 1 ? descriptor.sampleCount : 1;

        pipeline.vertexLayout = descriptor.vertexLayout;

        if (!device.isValid() || descriptor.library == nullptr)
            return;

        if (!device.supportsSampleCount(state.sampleCount))
        {
            LOG("OpenGL: no render pipeline at ",
                state.sampleCount,
                "x MSAA - the device does not support that sample count");
            return;
        }

        if (!glColorFormatIsRenderable(descriptor.colorFormat,
                                       context.getCapabilities()))
        {
            LOG("OpenGL: this context cannot attach that colour format, so the "
                "pipeline is invalid rather than refused at the first draw");
            return;
        }

        auto* library =
            static_cast<GLShaderLibraryData*>(descriptor.library->nativeLibrary());

        if (library == nullptr || library->vertex == 0 || library->fragment == 0)
            return;

        context.makeCurrent();
        link(*library);
    }

    ~GLRenderPipelineBackend() override
    {
        if (pipeline.program == 0 || !context.isValid())
            return;

        context.makeCurrent();
        glDeleteProgram(pipeline.program);
    }

    void link(const GLShaderLibraryData& library)
    {
        const auto program = glCreateProgram();

        if (program == 0)
            return;

        glAttachShader(program, library.vertex);
        glAttachShader(program, library.fragment);
        glLinkProgram(program);

        // Attached only for the link; the library owns them and outlives no
        // pipeline in particular.
        glDetachShader(program, library.vertex);
        glDetachShader(program, library.fragment);

        auto linked = GLint {GL_FALSE};
        glGetProgramiv(program, GL_LINK_STATUS, &linked);

        if (linked == GL_FALSE)
        {
            LOG("OpenGL: the program would not link for GLSL ",
                library.target.version,
                library.target.isES() ? " es: " : " core: ",
                glProgramLog(program));

            glDeleteProgram(program);
            return;
        }

        pipeline.program = program;
        resolveBindings(library);
    }

    // Every block and sampler by the name the linked program knows it by, which
    // is the only route below core 420 / ES 310 and costs nothing above it.
    void resolveBindings(const GLShaderLibraryData& library)
    {
        const auto program = pipeline.program;

        glUseProgram(program);

        for (const auto& binding: library.bindings)
        {
            if (bindUniformBlock(binding) || bindSampler(binding))
                continue;

            bindStorageBlock(binding);
        }

        pipeline.clipYSignLocation = glGetUniformLocation(program, "eacpClipYSign");

        glUseProgram(0);
        glDrainErrors("pipeline binding");
    }

    bool bindUniformBlock(const NamedBinding& binding)
    {
        const auto index =
            glGetUniformBlockIndex(pipeline.program, binding.name.c_str());

        if (index == GL_INVALID_INDEX)
            return false;

        const auto point = glUniformBindingPoint(binding.binding);

        glUniformBlockBinding(pipeline.program, index, (GLuint) point);

        auto size = GLint {0};
        glGetActiveUniformBlockiv(
            pipeline.program, index, GL_UNIFORM_BLOCK_DATA_SIZE, &size);

        if (size > pipeline.uniformBlockSize)
            pipeline.uniformBlockSize = size;

        pipeline.uniformBlocks.add({(GLint) index, point});

        return true;
    }

    // A sampler is a plain uniform, so the unit it reads is set once here rather
    // than at every bind.
    bool bindSampler(const NamedBinding& binding)
    {
        const auto location =
            glGetUniformLocation(pipeline.program, binding.name.c_str());

        if (location < 0)
            return false;

        const auto unit = glTextureUnitFor(binding.binding);

        glUniform1i(location, unit);
        pipeline.samplers.add({location, unit});

        return true;
    }

    void bindStorageBlock(const NamedBinding& binding)
    {
        if (!context.getCapabilities().storageBuffers)
            return;

        const auto index = glGetProgramResourceIndex(
            pipeline.program, GL_SHADER_STORAGE_BLOCK, binding.name.c_str());

        if (index == GL_INVALID_INDEX)
            return;

        const auto point = glStorageBindingPoint(binding.binding);

        glShaderStorageBlockBinding(pipeline.program, index, (GLuint) point);
        pipeline.storageBlocks.add({(GLint) index, point});
    }

    bool isValid() const override { return pipeline.isValid(); }

    PrimitiveTopology topology() const override { return topologyKind; }

    CullMode cullMode() const override { return cull; }

    Winding frontFace() const override { return winding; }

    void* nativeState() const override
    {
        return const_cast<GLRenderPipelineData*>(&pipeline);
    }

    void* nativeDepthState() const override
    {
        const auto tests = pipeline.state.depth || pipeline.state.stencil;

        return tests ? const_cast<GLRenderPipelineData*>(&pipeline) : nullptr;
    }

    GLContext& context;

    PrimitiveTopology topologyKind = PrimitiveTopology::Triangles;
    CullMode cull = CullMode::None;
    Winding winding = Winding::CounterClockwise;

    GLRenderPipelineData pipeline;
};
} // namespace

// The three binding spaces the Vulkan sources number into, mapped onto the
// three GL counts them from zero. A texture binding is maxTextureSlots + slot
// on both a render and a compute source, a render storage buffer is
// RenderPass::bufferBase + slot while a kernel's is the slot itself, and a
// kernel's uniform block sits at ComputePass::uniformBase where a render one
// is at zero - so each of these is "take the base off where there is one".
//
// It has to be done at all because GL's own counts are small: a driver need
// offer only eight shader-storage binding points, which binding 24 is past.
int glTextureUnitFor(int binding)
{
    return binding >= maxTextureSlots ? binding - maxTextureSlots : binding;
}

int glStorageBindingPoint(int binding)
{
    return binding >= RenderPass::bufferBase ? binding - RenderPass::bufferBase
                                             : binding;
}

int glUniformBindingPoint(int binding)
{
    return binding >= RenderPass::uniformBase ? binding - RenderPass::uniformBase
                                              : binding;
}

std::unique_ptr<RenderPipelineBackend>
    makeGLRenderPipeline(Device& device,
                         const RenderPipelineDescriptor& descriptor)
{
    return std::make_unique<GLRenderPipelineBackend>(device, descriptor);
}
} // namespace eacp::GPU
