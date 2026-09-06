#include "SpirvCompiler.h"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include <mutex>

namespace eacp::GPU::Spirv
{
namespace
{
constexpr auto glslVersion = 450;

// glslang's "input semantics version", not a GLSL version: 100 is what every
// Vulkan client uses and the only value the GLSL front end recognises.
constexpr auto inputSemanticsVersion = 100;

EShLanguage toLanguage(Stage stage)
{
    switch (stage)
    {
        case Stage::Fragment:
            return EShLangFragment;
        case Stage::Compute:
            return EShLangCompute;
        case Stage::Vertex:
            break;
    }

    return EShLangVertex;
}

// A preamble is a separate string glslang processes before the source without
// letting it invalidate the source's `#version` line, so the stage macro can be
// injected without editing the text or shifting the reported line numbers.
const char* stagePreamble(Stage stage)
{
    switch (stage)
    {
        case Stage::Vertex:
            return "#define EACP_VERTEX 1\n";
        case Stage::Fragment:
            return "#define EACP_FRAGMENT 1\n";
        case Stage::Compute:
            break;
    }

    return "";
}

EShMessages spirvMessages()
{
    return static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
}

void appendTo(std::string& log, const char* text)
{
    if (text == nullptr || *text == '\0')
        return;

    log += text;

    if (log.back() != '\n')
        log += '\n';
}

// Once per process and never finalized: FinalizeProcess frees the built-in
// symbol tables every later compile would rebuild, and any thread still inside
// glslang when it runs is reading freed memory.
void initializeGlslang()
{
    static auto once = std::once_flag {};
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

std::string warmUpSource()
{
    return "#version 450\n"
           "layout(local_size_x = 1) in;\n"
           "void main() {}\n";
}
} // namespace

CompileResult compileGlsl(Stage stage, const std::string& source)
{
    initializeGlslang();

    auto result = CompileResult {};

    const auto language = toLanguage(stage);
    auto shader = glslang::TShader {language};

    const auto* text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setPreamble(stagePreamble(stage));
    shader.setEntryPoint("main");
    shader.setEnvInput(glslang::EShSourceGlsl,
                       language,
                       glslang::EShClientVulkan,
                       inputSemanticsVersion);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

    if (!shader.parse(GetDefaultResources(), glslVersion, false, spirvMessages()))
    {
        appendTo(result.log, shader.getInfoLog());
        appendTo(result.log, shader.getInfoDebugLog());
        return result;
    }

    auto program = glslang::TProgram {};
    program.addShader(&shader);

    if (!program.link(spirvMessages()))
    {
        appendTo(result.log, program.getInfoLog());
        appendTo(result.log, program.getInfoDebugLog());
        return result;
    }

    auto logger = spv::SpvBuildLogger {};
    auto options = glslang::SpvOptions {};
    options.disableOptimizer = true;

    auto words = std::vector<unsigned int> {};
    glslang::GlslangToSpv(
        *program.getIntermediate(language), words, &logger, &options);

    appendTo(result.log, logger.getAllMessages().c_str());

    result.words.reserve(words.size());

    for (auto word: words)
        result.words.add(word);

    return result;
}

void warmUp()
{
    static auto once = std::once_flag {};
    std::call_once(once, [] { compileGlsl(Stage::Compute, warmUpSource()); });
}
} // namespace eacp::GPU::Spirv
