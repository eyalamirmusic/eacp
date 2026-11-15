#pragma once

#include "ShaderSource.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// What compiling a shader said, WebGPU's GPUCompilationInfo: one message per
// diagnostic, each with its severity, its text and where in the source it
// points. ShaderLibrary::compilationInfo() holds a library's - its errors, and
// the warnings of a compile that succeeded - and the parsers below are how each
// backend's compiler output becomes them, portable so every lane tests all of
// them against captured logs.

namespace eacp::GPU
{
enum class CompilationMessageType : std::uint8_t
{
    Error,
    Warning,
    Info
};

std::string_view compilationMessageTypeName(CompilationMessageType type);

struct CompilationMessage
{
    bool hasPosition() const;

    // "error 7:16 (fragment): text", for a log or a failed check.
    std::string describe() const;

    CompilationMessageType type = CompilationMessageType::Error;
    std::string message;

    // 1-based, in the source as the compiler was given it; 0 where the
    // compiler named no line or no column.
    int line = 0;
    int column = 0;

    // The columns the compiler marked from `column`, where it says (FXC's
    // (12,5-9) is five); 0 otherwise.
    int length = 0;

    // The stage whose compile reported it, where a library compiles each stage
    // on its own (GLSL, HLSL). Empty for a Metal library, compiled whole, and
    // for what a library reports of itself.
    std::optional<ShaderStage> stage;
};

struct CompilationInfo
{
    bool hasErrors() const;
    int countOf(CompilationMessageType type) const;

    CompilationInfo& add(CompilationMessageType type,
                         const std::string& text,
                         std::optional<ShaderStage> stage = {});
    CompilationInfo& append(const CompilationInfo& other);

    // Every message's describe(), one per line.
    std::string describe() const;

    Vector<CompilationMessage> messages;
};

// One compile's output, as a parser below takes it: the text, the stage it
// compiled where that was one stage, and whether it failed.
struct CompileLog
{
    std::string_view text;
    std::optional<ShaderStage> stage;
    bool failed = false;
};

// Each parser makes a message of every diagnostic line and drops what compilers
// print between them - the source excerpt and caret under a clang diagnostic,
// glslang's count of errors. A log with text none of whose lines is a
// diagnostic is one message of the whole text, trimmed, with no position: an
// error where the compile failed, a warning otherwise. An empty log is none.

// Clang's format, which the Metal compiler prints into the NSError of a library
// it failed: `program_source:12:5: error: text`, with warning, note, remark
// and fatal error beside error.
CompilationInfo parseMetalLog(const CompileLog& log);

// DXC prints the clang format too: `hlsl.hlsl:12:5: error: text`.
CompilationInfo parseDxcLog(const CompileLog& log);

// glslang's info log: `ERROR: 0:12: 'name' : text`, or `0:12:5:` with the
// column (SpirvCompiler asks for it), WARNING and NOTE beside ERROR; one with
// no position, as a link error is, stays one.
CompilationInfo parseGlslangLog(const CompileLog& log);

// FXC's: `file(12,5-9): error X3004: text`, or `error X3501: text` with no
// position. The code stays at the head of the text.
CompilationInfo parseFxcLog(const CompileLog& log);
} // namespace eacp::GPU
