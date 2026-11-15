#include "CompilationInfo.h"

#include <eacp/Core/Utils/Format.h>
#include <eacp/Core/Utils/Strings.h>

#include <array>

namespace eacp::GPU
{
namespace
{
struct CompileLogSeverity
{
    std::string_view word;
    CompilationMessageType type;
};

constexpr auto clangSeverities =
    std::array {CompileLogSeverity {"fatal error", CompilationMessageType::Error},
                CompileLogSeverity {"error", CompilationMessageType::Error},
                CompileLogSeverity {"warning", CompilationMessageType::Warning},
                CompileLogSeverity {"note", CompilationMessageType::Info},
                CompileLogSeverity {"remark", CompilationMessageType::Info}};

constexpr auto glslangSeverities =
    std::array {CompileLogSeverity {"INTERNAL ERROR", CompilationMessageType::Error},
                CompileLogSeverity {"UNKNOWN ERROR", CompilationMessageType::Error},
                CompileLogSeverity {"UNIMPLEMENTED", CompilationMessageType::Error},
                CompileLogSeverity {"ERROR", CompilationMessageType::Error},
                CompileLogSeverity {"WARNING", CompilationMessageType::Warning},
                CompileLogSeverity {"NOTE", CompilationMessageType::Info}};

constexpr auto fxcSeverities =
    std::array {CompileLogSeverity {"error", CompilationMessageType::Error},
                CompileLogSeverity {"warning", CompilationMessageType::Warning}};

std::optional<int> compileLogNumber(std::string_view text)
{
    if (text.empty())
        return std::nullopt;

    return Strings::tryParse<int>(text);
}

std::string_view compileLogTrimmedLine(std::string_view line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.remove_suffix(1);

    return line;
}

CompilationMessage compileLogMessageOf(const CompileLog& log,
                                       CompilationMessageType type,
                                       std::string_view text)
{
    auto message = CompilationMessage {};
    message.type = type;
    message.message = Strings::trim(text);
    message.stage = log.stage;
    return message;
}

// "file:12:5" or "file:12", read from the right so a Windows drive letter's
// colon is part of the file.
void readClangLocation(std::string_view location, CompilationMessage& message)
{
    auto last = location.rfind(':');

    if (last == std::string_view::npos)
        return;

    auto lastNumber = compileLogNumber(location.substr(last + 1));

    if (!lastNumber)
        return;

    auto rest = location.substr(0, last);
    auto previous = rest.rfind(':');

    if (previous != std::string_view::npos)
    {
        if (auto line = compileLogNumber(rest.substr(previous + 1)))
        {
            message.line = *line;
            message.column = *lastNumber;
            return;
        }
    }

    message.line = *lastNumber;
}

// The severity marker nearest the start, so a message quoting another
// severity's word is not cut at it.
std::optional<CompilationMessage> parseClangLine(const CompileLog& log,
                                                 std::string_view line)
{
    for (const auto& severity: clangSeverities)
    {
        auto bare = Strings::concat(severity.word, ": ");

        if (line.starts_with(bare))
            return compileLogMessageOf(log, severity.type, line.substr(bare.size()));
    }

    auto found = std::string_view::npos;
    auto markerSize = std::size_t {0};
    auto type = CompilationMessageType::Error;

    for (const auto& severity: clangSeverities)
    {
        auto marker = Strings::concat(": ", severity.word, ": ");
        auto at = line.find(marker);

        if (at < found)
        {
            found = at;
            markerSize = marker.size();
            type = severity.type;
        }
    }

    if (found == std::string_view::npos)
        return std::nullopt;

    auto message = compileLogMessageOf(log, type, line.substr(found + markerSize));
    readClangLocation(line.substr(0, found), message);
    return message;
}

// glslang's "0:12: " or "0:12:5: " after the prefix: a string number, then
// the line and, when asked for, the column.
std::string_view readGlslangLocation(std::string_view text,
                                     CompilationMessage& message)
{
    auto end = text.find(": ");

    if (end == std::string_view::npos)
        return text;

    auto parts = Strings::split(text.substr(0, end), ':');

    if (parts.size() < 2 || parts.size() > 3)
        return text;

    auto numbers = Vector<int> {};

    for (const auto& part: parts)
    {
        auto number = compileLogNumber(part);

        if (!number)
            return text;

        numbers.add(*number);
    }

    message.line = numbers[1];
    message.column = numbers.size() == 3 ? numbers[2] : 0;
    return text.substr(end + 2);
}

bool isGlslangSummary(std::string_view text)
{
    return text.find("compilation errors.") != std::string_view::npos
           && text.find("No code generated.") != std::string_view::npos;
}

std::optional<CompilationMessage> parseGlslangLine(const CompileLog& log,
                                                   std::string_view line)
{
    for (const auto& severity: glslangSeverities)
    {
        auto prefix = Strings::concat(severity.word, ": ");

        if (!line.starts_with(prefix))
            continue;

        auto rest = line.substr(prefix.size());

        if (isGlslangSummary(rest))
            return std::nullopt;

        auto message = compileLogMessageOf(log, severity.type, {});
        message.message = Strings::trim(readGlslangLocation(rest, message));
        return message;
    }

    return std::nullopt;
}

// "(12,5-9)", "(12,5)" or "(12)".
void readFxcLocation(std::string_view location, CompilationMessage& message)
{
    auto open = location.rfind('(');

    if (open == std::string_view::npos || location.back() != ')')
        return;

    auto inside = location.substr(open + 1, location.size() - open - 2);
    auto parts = Strings::split(inside, ',');

    auto line = compileLogNumber(parts[0]);

    if (!line)
        return;

    message.line = *line;

    if (parts.size() != 2)
        return;

    auto columns = Strings::split(parts[1], '-');

    if (auto column = compileLogNumber(columns[0]))
        message.column = *column;

    if (columns.size() != 2)
        return;

    if (auto last = compileLogNumber(columns[1]); last && *last >= message.column)
        message.length = *last - message.column + 1;
}

std::optional<CompilationMessage> parseFxcLine(const CompileLog& log,
                                               std::string_view line)
{
    for (const auto& severity: fxcSeverities)
    {
        auto located = Strings::concat("): ", severity.word);
        auto at = line.find(located);
        auto location = std::string_view {};
        auto rest = std::string_view {};

        if (at != std::string_view::npos)
        {
            location = line.substr(0, at + 1);
            rest = line.substr(at + located.size());
        }
        else if (line.starts_with(severity.word))
        {
            rest = line.substr(severity.word.size());
        }
        else
        {
            continue;
        }

        if (rest.empty() || (rest.front() != ' ' && rest.front() != ':'))
            continue;

        auto message = compileLogMessageOf(log, severity.type, rest);

        if (message.message.starts_with(':'))
            message.message = Strings::trim(message.message.substr(1));

        if (!location.empty())
            readFxcLocation(location, message);

        return message;
    }

    return std::nullopt;
}

template <typename ParseLine>
CompilationInfo parseCompileLog(const CompileLog& log, const ParseLine& parseLine)
{
    auto info = CompilationInfo {};

    for (const auto& line: Strings::split(log.text, '\n'))
        if (auto message = parseLine(log, compileLogTrimmedLine(line)))
            info.messages.add(*message);

    auto whole = Strings::trim(log.text);

    if (info.messages.empty() && !whole.empty())
        info.add(log.failed ? CompilationMessageType::Error
                            : CompilationMessageType::Warning,
                 whole,
                 log.stage);

    return info;
}

std::string_view stageNameForMessage(ShaderStage stage)
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
} // namespace

std::string_view compilationMessageTypeName(CompilationMessageType type)
{
    switch (type)
    {
        case CompilationMessageType::Error:
            return "error";
        case CompilationMessageType::Warning:
            return "warning";
        case CompilationMessageType::Info:
            break;
    }

    return "info";
}

bool CompilationMessage::hasPosition() const
{
    return line > 0;
}

std::string CompilationMessage::describe() const
{
    auto text = std::string(compilationMessageTypeName(type));

    if (hasPosition())
        text += column > 0 ? Strings::format(" {}:{}", line, column)
                           : Strings::format(" {}", line);

    if (stage)
        text += Strings::format(" ({})", stageNameForMessage(*stage));

    return Strings::concat(text, ": ", message);
}

bool CompilationInfo::hasErrors() const
{
    return countOf(CompilationMessageType::Error) > 0;
}

int CompilationInfo::countOf(CompilationMessageType type) const
{
    auto count = 0;

    for (const auto& message: messages)
        if (message.type == type)
            ++count;

    return count;
}

CompilationInfo& CompilationInfo::add(CompilationMessageType type,
                                      const std::string& text,
                                      std::optional<ShaderStage> stage)
{
    auto message = CompilationMessage {};
    message.type = type;
    message.message = text;
    message.stage = stage;
    messages.add(message);
    return *this;
}

CompilationInfo& CompilationInfo::append(const CompilationInfo& other)
{
    for (const auto& message: other.messages)
        messages.add(message);

    return *this;
}

std::string CompilationInfo::describe() const
{
    auto text = std::string {};

    for (const auto& message: messages)
        text += message.describe() + "\n";

    return text;
}

CompilationInfo parseMetalLog(const CompileLog& log)
{
    return parseCompileLog(log, parseClangLine);
}

CompilationInfo parseDxcLog(const CompileLog& log)
{
    return parseCompileLog(log, parseClangLine);
}

CompilationInfo parseGlslangLog(const CompileLog& log)
{
    return parseCompileLog(log, parseGlslangLine);
}

CompilationInfo parseFxcLog(const CompileLog& log)
{
    return parseCompileLog(log, parseFxcLine);
}
} // namespace eacp::GPU
