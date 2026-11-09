#include "CodegenCommon.h"

#include <eacp/GPU/Shader/CompilationInfo.h>

#include <string>
#include <string_view>

// The compiler-output parsers behind ShaderLibrary::compilationInfo(), fed
// logs each backend's compiler printed. The Metal and glslang ones are real
// output, captured from the Metal compiler on macOS and from eacp-spirv's
// glslang; the FXC and DXC ones follow those compilers' documented formats,
// since nothing here has run them. Where eacp-spirv is built, glslang's live
// output is parsed too. What a library on the device reports is
// CompilationInfoDevice/'s, in GPUTests.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace CompilationInfoTests
{
namespace
{
constexpr auto metalErrors = std::string_view {
    "program_source:7:16: error: use of undeclared identifier 'undeclaredThing'\n"
    "    out[gid] = undeclaredThing * 2.0f;\n"
    "               ^\n"
    "program_source:8:15: error: expected expression\n"
    "    float y = ;\n"
    "              ^\n"};

constexpr auto metalWarningAndError = std::string_view {
    "program_source:3:2: warning: eacp sample warning [-W#warnings]\n"
    "#warning eacp sample warning\n"
    " ^\n"
    "program_source:7:16: error: use of undeclared identifier 'missing'; did you "
    "mean 'using'?\n"
    "    out[gid] = missing;\n"
    "               ^~~~~~~\n"
    "               using\n"
    "program_source:7:16: error: expected expression\n"};

constexpr auto glslangErrors =
    std::string_view {"ERROR: 0:7:49: 'undeclaredThing' : undeclared identifier \n"
                      "ERROR: 0:7:65: '' : compilation terminated \n"
                      "ERROR: 2 compilation errors.  No code generated.\n"
                      "\n"};

constexpr auto glslangWarning =
    std::string_view {"WARNING: 0:2:42: '#extension' : extension not supported: "
                      "GL_EXT_eacp_nonexistent\n"};

constexpr auto glslangWithoutColumns =
    std::string_view {"ERROR: 0:12: 'foo' : undeclared identifier \n"
                      "WARNING: 0:3: 'bar' : something to note \n"
                      "ERROR: 2 compilation errors.  No code generated.\n"};

constexpr auto glslangLinkError = std::string_view {
    "ERROR: Linking vertex stage: Missing entry point: Each stage requires one "
    "entry point\n"
    "\n"};

constexpr auto fxcLog = std::string_view {
    "C:\\Users\\eacp\\Shader@0x000001E8C4F6B0A0(6,21-35): error X3004: "
    "undeclared identifier 'undeclaredThing'\n"
    "C:\\Users\\eacp\\Shader@0x000001E8C4F6B0A0(9,16-34): warning X3206: "
    "implicit truncation of vector type\n"
    "error X3501: 'computeMain': entrypoint not found\n"};

constexpr auto dxcLog = std::string_view {
    "hlsl.hlsl:6:21: error: use of undeclared identifier 'undeclaredThing'\n"
    "    output[gid.x] = undeclaredThing * 2.0f;\n"
    "                    ^\n"
    "hlsl.hlsl:9:16: warning: implicit truncation of vector type [-Wconversion]\n"
    "    float2 v = float4(1, 2, 3, 4);\n"
    "               ^\n"
    "C:\\Shaders\\common.hlsli:3:1: note: declared here\n"
    "1 error generated.\n"};

// Message `index`, or an empty one past the end, so a check that fails on the
// count does not take the rest of the suite down with it.
const CompilationMessage& messageAt(const CompilationInfo& info, int index)
{
    static const auto missing = CompilationMessage {};
    return index < info.messages.size() ? info.messages[index] : missing;
}

void checkMessage(
    const CompilationMessage& message,
    CompilationMessageType type,
    int line,
    int column,
    std::string_view text,
    const std::source_location& where = std::source_location::current())
{
    check(message.type == type, message.describe(), where);
    check(message.line == line, message.describe(), where);
    check(message.column == column, message.describe(), where);
    check(message.message == text, message.describe(), where);
}

auto tMetal = test("CompilationInfo/metalErrorsCarryLineAndColumn") = []
{
    const auto info = parseMetalLog({metalErrors, {}, true});

    check(info.messages.size() == 2, info.describe());
    checkMessage(messageAt(info, 0),
                 CompilationMessageType::Error,
                 7,
                 16,
                 "use of undeclared identifier 'undeclaredThing'");
    checkMessage(messageAt(info, 1),
                 CompilationMessageType::Error,
                 8,
                 15,
                 "expected expression");
    check(info.hasErrors());
    check(!messageAt(info, 0).stage.has_value(), "a Metal library has no stage");
};

auto tMetalWarning = test("CompilationInfo/metalKeepsAWarningBesideItsErrors") = []
{
    const auto info = parseMetalLog({metalWarningAndError, {}, true});

    check(info.messages.size() == 3, info.describe());
    checkMessage(messageAt(info, 0),
                 CompilationMessageType::Warning,
                 3,
                 2,
                 "eacp sample warning [-W#warnings]");
    checkMessage(messageAt(info, 1),
                 CompilationMessageType::Error,
                 7,
                 16,
                 "use of undeclared identifier 'missing'; did you mean 'using'?");
    check(info.countOf(CompilationMessageType::Warning) == 1);
    check(info.countOf(CompilationMessageType::Error) == 2);
};

auto tGlslang = test("CompilationInfo/glslangDropsItsSummaryLine") = []
{
    const auto info = parseGlslangLog({glslangErrors, ShaderStage::Compute, true});

    check(info.messages.size() == 2, info.describe());
    checkMessage(messageAt(info, 0),
                 CompilationMessageType::Error,
                 7,
                 49,
                 "'undeclaredThing' : undeclared identifier");
    checkMessage(messageAt(info, 1),
                 CompilationMessageType::Error,
                 7,
                 65,
                 "'' : compilation terminated");
    check(messageAt(info, 0).stage == ShaderStage::Compute);
};

auto tGlslangWarning = test("CompilationInfo/glslangWarningsOnASuccess") = []
{
    const auto info =
        parseGlslangLog({glslangWarning, ShaderStage::Fragment, false});

    check(info.messages.size() == 1, info.describe());
    checkMessage(messageAt(info, 0),
                 CompilationMessageType::Warning,
                 2,
                 42,
                 "'#extension' : extension not supported: GL_EXT_eacp_nonexistent");
    check(!info.hasErrors());
    check(messageAt(info, 0).stage == ShaderStage::Fragment);
};

auto tGlslangLines = test("CompilationInfo/glslangWithoutColumns") = []
{
    const auto info = parseGlslangLog({glslangWithoutColumns, ShaderStage::Vertex});

    check(info.messages.size() == 2, info.describe());
    checkMessage(messageAt(info, 0),
                 CompilationMessageType::Error,
                 12,
                 0,
                 "'foo' : undeclared identifier");
    checkMessage(messageAt(info, 1),
                 CompilationMessageType::Warning,
                 3,
                 0,
                 "'bar' : something to note");
};

auto tGlslangLink = test("CompilationInfo/aGlslangLinkErrorHasNoPosition") = []
{
    const auto info = parseGlslangLog({glslangLinkError, ShaderStage::Vertex, true});

    check(info.messages.size() == 1, info.describe());
    checkMessage(messageAt(info, 0),
                 CompilationMessageType::Error,
                 0,
                 0,
                 "Linking vertex stage: Missing entry point: Each stage requires "
                 "one entry point");
    check(!messageAt(info, 0).hasPosition());
};

auto tFxc = test("CompilationInfo/fxcLinesColumnsAndSpans") = []
{
    const auto info = parseFxcLog({fxcLog, ShaderStage::Compute, true});

    check(info.messages.size() == 3, info.describe());
    checkMessage(messageAt(info, 0),
                 CompilationMessageType::Error,
                 6,
                 21,
                 "X3004: undeclared identifier 'undeclaredThing'");
    check(messageAt(info, 0).length == 15, messageAt(info, 0).describe());
    checkMessage(messageAt(info, 1),
                 CompilationMessageType::Warning,
                 9,
                 16,
                 "X3206: implicit truncation of vector type");
    check(messageAt(info, 1).length == 19);
    checkMessage(messageAt(info, 2),
                 CompilationMessageType::Error,
                 0,
                 0,
                 "X3501: 'computeMain': entrypoint not found");
};

auto tDxc = test("CompilationInfo/dxcIsClangsFormat") = []
{
    const auto info = parseDxcLog({dxcLog, ShaderStage::Fragment, true});

    check(info.messages.size() == 3, info.describe());
    checkMessage(messageAt(info, 0),
                 CompilationMessageType::Error,
                 6,
                 21,
                 "use of undeclared identifier 'undeclaredThing'");
    checkMessage(messageAt(info, 1),
                 CompilationMessageType::Warning,
                 9,
                 16,
                 "implicit truncation of vector type [-Wconversion]");
    checkMessage(
        messageAt(info, 2), CompilationMessageType::Info, 3, 1, "declared here");
    check(messageAt(info, 2).stage == ShaderStage::Fragment);
};

auto tUnparsed = test("CompilationInfo/anUnparsedLogIsOneMessage") = []
{
    const auto failed =
        parseMetalLog({"  Compilation failed\n\nan internal error  \n", {}, true});

    check(failed.messages.size() == 1, failed.describe());
    checkMessage(messageAt(failed, 0),
                 CompilationMessageType::Error,
                 0,
                 0,
                 "Compilation failed\n\nan internal error");

    const auto succeeded =
        parseFxcLog({"something the compiler said", ShaderStage::Vertex, false});

    check(succeeded.messages.size() == 1);
    check(messageAt(succeeded, 0).type == CompilationMessageType::Warning,
          "a log that does not parse beside a success is a warning");
    check(messageAt(succeeded, 0).stage == ShaderStage::Vertex);

    check(parseGlslangLog({"", ShaderStage::Compute, true}).messages.empty());
    check(parseDxcLog({" \n\r\n", {}, false}).messages.empty());
};

auto tSeverityWords = test("CompilationInfo/theEarliestSeverityWins") = []
{
    const auto info = parseMetalLog(
        {"C:\\dir\\a.metal:4:9: warning: an error: quoted in the text\n"
         "fatal error: no input files\n"
         "b.metal:2: remark: a line and no column\n",
         {},
         false});

    check(info.messages.size() == 3, info.describe());
    checkMessage(messageAt(info, 0),
                 CompilationMessageType::Warning,
                 4,
                 9,
                 "an error: quoted in the text");
    checkMessage(
        messageAt(info, 1), CompilationMessageType::Error, 0, 0, "no input files");
    checkMessage(messageAt(info, 2),
                 CompilationMessageType::Info,
                 2,
                 0,
                 "a line and no column");
};

auto tDescribe = test("CompilationInfo/describeNamesEverything") = []
{
    auto info = CompilationInfo {};
    info.add(CompilationMessageType::Warning, "plain");

    auto positioned = CompilationMessage {};
    positioned.message = "bad";
    positioned.line = 7;
    positioned.column = 16;
    positioned.stage = ShaderStage::Fragment;
    info.messages.add(positioned);

    check(info.describe() == "warning: plain\nerror 7:16 (fragment): bad\n",
          info.describe());

    auto other = CompilationInfo {};
    other.add(CompilationMessageType::Info, "more", ShaderStage::Compute);
    info.append(other);

    check(info.messages.size() == 3);
    check(info.countOf(CompilationMessageType::Info) == 1);
    check(info.hasErrors());
};

#ifdef EACP_HAS_SPIRV
constexpr auto brokenKernel = std::string_view {
    "#version 450\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Out { float values[]; } outBuffer;\n"
    "\n"
    "void main()\n"
    "{\n"
    "    outBuffer.values[gl_GlobalInvocationID.x] = undeclaredThing * 2.0;\n"
    "}\n"};

constexpr auto warnedKernel = std::string_view {
    "#version 450\n"
    "#extension GL_EXT_eacp_nonexistent : warn\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer Out { float values[]; } outBuffer;\n"
    "\n"
    "void main()\n"
    "{\n"
    "    outBuffer.values[gl_GlobalInvocationID.x] = 2.0;\n"
    "}\n"};

auto tLiveGlslang = test("CompilationInfo/glslangsOwnOutputParses") = []
{
    using Spirv::Stage;

    const auto broken =
        Spirv::compileGlsl(Stage::Compute, std::string(brokenKernel));
    check(!broken.succeeded());
    check(broken.log == glslangErrors, broken.log);

    const auto errors = parseGlslangLog({broken.log, ShaderStage::Compute, true});
    check(errors.hasErrors(), errors.describe());
    checkMessage(messageAt(errors, 0),
                 CompilationMessageType::Error,
                 7,
                 49,
                 "'undeclaredThing' : undeclared identifier");

    const auto warned =
        Spirv::compileGlsl(Stage::Compute, std::string(warnedKernel));
    check(warned.succeeded(), warned.log);
    check(warned.log == glslangWarning, warned.log);

    const auto warnings = parseGlslangLog({warned.log, ShaderStage::Compute, false});
    check(warnings.messages.size() == 1, warnings.describe());
    check(messageAt(warnings, 0).type == CompilationMessageType::Warning);
    check(messageAt(warnings, 0).line == 2);
    check(messageAt(warnings, 0).column == 42);
};
#endif
} // namespace
} // namespace CompilationInfoTests
