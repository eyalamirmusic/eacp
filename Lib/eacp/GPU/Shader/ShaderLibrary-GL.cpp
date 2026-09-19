#include "ShaderLibrary.h"

#include "../Codegen/GlslLowering.h"
#include "../Device/Device.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"
#include "ShaderSource.h"

#include <eacp/Core/Utils/Logging.h>

#include <string>

// The entry names ShaderSource carries are ignored: a lowered stage's entry is
// main, which is what glShaderSource is handed and what the D7 wrapper renames
// the emitted one to.

namespace eacp::GPU
{
namespace
{
std::string glShaderLog(GLuint shader)
{
    auto length = GLint {0};
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);

    if (length <= 1)
        return {};

    auto log = std::string((std::size_t) length, '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());

    // The driver's own terminator, which a std::string does not want.
    while (!log.empty() && log.back() == '\0')
        log.pop_back();

    return log;
}

GLenum glShaderTypeFor(ShaderStage stage)
{
    switch (stage)
    {
        case ShaderStage::Vertex:
            return GL_VERTEX_SHADER;

        case ShaderStage::Fragment:
            return GL_FRAGMENT_SHADER;

        case ShaderStage::Compute:
            break;
    }

    return GL_COMPUTE_SHADER;
}

const char* glStageName(ShaderStage stage)
{
    switch (stage)
    {
        case ShaderStage::Vertex:
            return "vertex";

        case ShaderStage::Fragment:
            return "fragment";

        case ShaderStage::Compute:
            break;
    }

    return "compute";
}

struct GLShaderLibraryBackend final : ShaderLibraryBackend
{
    GLShaderLibraryBackend(Device& device, const ShaderSource& source)
        : context(getGLContext(device))
    {
        // An empty source is a build something declined to make, and whatever
        // declined it has already said why - see ComputeProgram::prepare.
        if (!device.isValid() || source.source.empty())
            return;

        context.makeCurrent();
        program.target = context.getCapabilities().glslTarget();

        if (source.isCompute())
        {
            compileStage(ShaderStage::Compute, source.source, program.compute);
            return;
        }

        // Two compiles of one source, each standalone: the lowering defines the
        // stage macro the pair of #ifdefs is written against, so what reaches
        // glShaderSource is one stage with no preamble.
        compileStage(ShaderStage::Vertex, source.source, program.vertex);
        compileStage(ShaderStage::Fragment, source.source, program.fragment);
    }

    ~GLShaderLibraryBackend() override
    {
        if (!context.isValid())
            return;

        context.makeCurrent();

        for (auto shader: {program.vertex, program.fragment, program.compute})
            if (shader != 0)
                glDeleteShader(shader);
    }

    void compileStage(ShaderStage stage, const std::string& source, GLuint& shader)
    {
        const auto lowered = lowerGlsl(source, program.target, stage);

        if (!lowered.succeeded())
        {
            // "This device cannot run this shader" is not the same as "this is
            // not eacp's GLSL", and only the second is a source to go and fix.
            LOG("OpenGL: the ",
                glStageName(stage),
                " stage was not lowered for GLSL ",
                program.target.version,
                program.target.isES() ? " es" : " core",
                lowered.needsNewerTarget ? " (this context is too old for it): "
                                         : ": ",
                lowered.error);
            return;
        }

        shader = glCreateShader(glShaderTypeFor(stage));

        if (shader == 0)
            return;

        const auto* text = lowered.source.c_str();
        glShaderSource(shader, 1, &text, nullptr);
        glCompileShader(shader);

        auto compiled = GLint {GL_FALSE};
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

        if (const auto log = glShaderLog(shader); !log.empty())
            LOG("OpenGL ", glStageName(stage), " shader: ", log);

        if (compiled == GL_FALSE)
        {
            glDeleteShader(shader);
            shader = 0;
            return;
        }

        // Recorded whatever the target let the layout() keep: the pipeline
        // binds these by name after linking, which is the only route below
        // core 420 / ES 310 and costs nothing above it.
        for (const auto& binding: lowered.bindings)
            addBinding(binding);
    }

    void addBinding(const NamedBinding& binding)
    {
        for (const auto& known: program.bindings)
            if (known.name == binding.name)
                return;

        program.bindings.add(binding);
    }

    bool isValid() const override
    {
        if (program.compute != 0)
            return true;

        return program.vertex != 0 && program.fragment != 0;
    }

    void* nativeLibrary() const override
    {
        return const_cast<GLShaderLibraryData*>(&program);
    }

    GLContext& context;
    GLShaderLibraryData program;
};
} // namespace

std::unique_ptr<ShaderLibraryBackend> makeGLShaderLibrary(Device& device,
                                                          const ShaderSource& source)
{
    return std::make_unique<GLShaderLibraryBackend>(device, source);
}
} // namespace eacp::GPU
