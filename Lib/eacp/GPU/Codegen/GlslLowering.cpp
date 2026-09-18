#include "GlslLowering.h"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace eacp::GPU
{
namespace
{
constexpr auto emittedVersion = "#version 450";

bool isIdentifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// A whole identifier rather than a substring, so a name that merely contains
// another is not mistaken for it.
bool containsToken(std::string_view text, std::string_view token)
{
    for (auto at = text.find(token); at != std::string_view::npos;
         at = text.find(token, at + 1))
    {
        auto before = at == 0 || !isIdentifierChar(text[at - 1]);
        auto afterAt = at + token.size();
        auto after = afterAt >= text.size() || !isIdentifierChar(text[afterAt]);

        if (before && after)
            return true;
    }

    return false;
}

// The subgroup builtins and calls are one family, so the prefix is the test.
bool containsIdentifierStartingWith(std::string_view text, std::string_view prefix)
{
    for (auto at = text.find(prefix); at != std::string_view::npos;
         at = text.find(prefix, at + 1))
        if (at == 0 || !isIdentifierChar(text[at - 1]))
            return true;

    return false;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);

    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);

    return text;
}

Vector<std::string> splitLines(std::string_view text)
{
    auto lines = Vector<std::string> {};
    auto start = std::size_t {0};

    while (start <= text.size())
    {
        auto end = text.find('\n', start);

        if (end == std::string_view::npos)
        {
            lines.add(std::string {text.substr(start)});
            break;
        }

        lines.add(std::string {text.substr(start, end - start)});
        start = end + 1;
    }

    return lines;
}

std::string versionLine(GlslTarget target)
{
    auto profile = target.isES() ? " es" : " core";
    return "#version " + std::to_string(target.version) + profile;
}

const char* stageMacro(ShaderStage stage)
{
    switch (stage)
    {
        case ShaderStage::Vertex:
            return "#define EACP_VERTEX 1\n";
        case ShaderStage::Fragment:
            return "#define EACP_FRAGMENT 1\n";
        case ShaderStage::Compute:
            break;
    }

    return "";
}

// Which half of a two-stage source a line sits in. A single-stage source has
// no such directives and the stage being lowered answers instead.
enum class Section
{
    None,
    Vertex,
    Fragment
};

Section sectionOf(std::string_view line, Section current)
{
    auto directive = trim(line);

    if (directive == "#ifdef EACP_VERTEX")
        return Section::Vertex;

    if (directive == "#ifdef EACP_FRAGMENT")
        return Section::Fragment;

    if (directive == "#endif")
        return Section::None;

    return current;
}

std::string qualifierName(std::string_view qualifier)
{
    return std::string {trim(qualifier.substr(0, qualifier.find('=')))};
}

int qualifierValue(std::string_view qualifier)
{
    auto assign = qualifier.find('=');

    if (assign == std::string_view::npos)
        return -1;

    return std::atoi(std::string {trim(qualifier.substr(assign + 1))}.c_str());
}

// The name the linked program knows the declaration by: the block name of a
// uniform or storage block, the variable name of a sampler or an image. Both
// are the last identifier before the body or the semicolon.
std::string declaredName(std::string_view declaration)
{
    auto head = declaration.substr(0, declaration.find_first_of("{;"));
    auto name = std::string {};
    auto token = std::string {};

    for (auto c: head)
    {
        if (isIdentifierChar(c))
        {
            token += c;
            continue;
        }

        if (!token.empty())
            name = std::exchange(token, std::string {});
    }

    return token.empty() ? name : token;
}

bool keepsLocation(std::string_view declaration, Section section, ShaderStage stage)
{
    auto isVertex = section == Section::Vertex
                    || (section == Section::None && stage == ShaderStage::Vertex);

    if (isVertex)
        return containsToken(declaration, "in");

    auto isFragment = section == Section::Fragment
                      || (section == Section::None
                          && stage == ShaderStage::Fragment);

    return isFragment ? containsToken(declaration, "out") : true;
}

struct LayoutRewrite
{
    std::string line;
    bool hasBinding = false;
    std::string name;
    int binding = 0;
};

LayoutRewrite rewriteLayout(const std::string& line,
                            GlslTarget target,
                            Section section,
                            ShaderStage stage)
{
    auto result = LayoutRewrite {};
    result.line = line;

    auto open = line.find("layout(");

    if (open == std::string::npos)
        return result;

    auto listStart = open + std::string_view {"layout("}.size();
    auto close = line.find(')', listStart);

    if (close == std::string::npos)
        return result;

    auto prefix = line.substr(0, open);
    auto declaration = line.substr(close + 1);
    auto list = std::string_view {line}.substr(listStart, close - listStart);

    auto kept = std::string {};

    auto keep = [&](std::string_view qualifier)
    {
        if (!kept.empty())
            kept += ", ";

        kept += trim(qualifier);
    };

    for (auto start = std::size_t {0}; start <= list.size();)
    {
        auto comma = list.find(',', start);
        auto qualifier = list.substr(start, comma == std::string_view::npos
                                                ? std::string_view::npos
                                                : comma - start);
        start = comma == std::string_view::npos ? list.size() + 1 : comma + 1;

        auto name = qualifierName(qualifier);

        if (name == "set")
            continue;

        if (name == "binding")
        {
            result.hasBinding = true;
            result.binding = qualifierValue(qualifier);
            result.name = declaredName(declaration);

            if (!target.allowsExplicitBindings())
                continue;
        }
        else if (name == "location" && !keepsLocation(declaration, section, stage))
        {
            continue;
        }

        keep(qualifier);
    }

    if (kept.empty())
    {
        auto rest = std::string_view {declaration};

        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);

        result.line = prefix + std::string {rest};
        return result;
    }

    result.line = prefix + "layout(" + kept + ")" + declaration;
    return result;
}

// GLSL only took brace initializers in core 420 and ES has never had them, so
// a constant array is declared the way every version reads it:
// vec3 a0[4] = vec3[4](...).
std::string rewriteArrayInitializer(const std::string& line)
{
    auto braces = line.find("] = {");

    if (braces == std::string::npos || !trim(line).ends_with("};"))
        return line;

    auto declaration = trim(std::string_view {line}.substr(0, braces));

    if (declaration.starts_with("const "))
        declaration.remove_prefix(std::string_view {"const "}.size());

    auto typeEnd = declaration.find(' ');
    auto sizeAt = declaration.rfind('[');

    if (typeEnd == std::string_view::npos || sizeAt == std::string_view::npos)
        return line;

    auto type = std::string {declaration.substr(0, typeEnd)};
    auto size = std::string {declaration.substr(sizeAt + 1)};

    auto open = line.find('{', braces);
    auto close = line.rfind('}');
    auto elements = line.substr(open + 1, close - open - 1);

    return line.substr(0, open) + type + "[" + size + "](" + elements + ")"
           + line.substr(close + 1);
}

// Everything the pass would otherwise pass through as if it were portable.
// Vulkan spells these and GL has no equivalent, so a source carrying one is
// not eacp's GLSL and says so rather than failing at the draw.
std::string unrecognisedConstruct(std::string_view source)
{
    for (auto builtin: {"gl_VertexIndex",
                        "gl_InstanceIndex",
                        "gl_BaseVertex",
                        "gl_BaseInstance"})
        if (containsToken(source, builtin))
            return std::string {builtin} + " has no OpenGL spelling";

    if (containsToken(source, "push_constant"))
        return "a push constant block has no OpenGL equivalent";

    if (containsIdentifierStartingWith(source, "subgroup")
        || containsIdentifierStartingWith(source, "gl_Subgroup"))
        return "a subgroup operation has no OpenGL equivalent";

    if (source.find("#extension") != std::string_view::npos)
        return "an #extension directive is the target's to decide";

    return {};
}

std::string tooOldForSource(std::string_view source,
                            GlslTarget target,
                            ShaderStage stage)
{
    if (stage == ShaderStage::Compute && !target.allowsCompute())
        return "a kernel needs core 430 or ES 310";

    if (containsToken(source, "std430") && !target.allowsStorageBuffers())
        return "a storage buffer needs core 430 or ES 310";

    if (containsToken(source, "image2D") && !target.allowsImageStore())
        return "an image needs core 420 or ES 310";

    if ((containsToken(source, "packHalf2x16")
         || containsToken(source, "unpackHalf2x16"))
        && !target.allowsHalfPacking())
        return "half packing needs core 420 or ES 300";

    return {};
}

LoweredGlsl failure(std::string error, bool needsNewerTarget = false)
{
    auto result = LoweredGlsl {};
    result.error = std::move(error);
    result.needsNewerTarget = needsNewerTarget;

    return result;
}

// GL rasterizes NDC +1 into the highest row of the target, where eacp puts the
// top of the picture. A pass on a texture hands the sign -1 and the picture
// lands the way every other backend writes it (plan.md D7).
constexpr auto clipYWrapper = "\nuniform float eacpClipYSign;\n"
                              "\n"
                              "void main()\n"
                              "{\n"
                              "    eacpMain();\n"
                              "    gl_Position.y *= eacpClipYSign;\n"
                              "}\n";

bool renameEntryPoint(std::string& source)
{
    constexpr auto entry = std::string_view {"void main()"};
    auto renamed = false;

    for (auto at = source.find(entry); at != std::string::npos;
         at = source.find(entry, at + 1))
    {
        source.replace(at, entry.size(), "void eacpMain()");
        renamed = true;
    }

    return renamed;
}
} // namespace

LoweredGlsl
    lowerGlsl(std::string_view vulkanGlsl, GlslTarget target, ShaderStage stage)
{
    if (auto reason = unrecognisedConstruct(vulkanGlsl); !reason.empty())
        return failure("eacp: " + reason);

    if (auto reason = tooOldForSource(vulkanGlsl, target, stage); !reason.empty())
        return failure("eacp: " + reason, true);

    auto lines = splitLines(vulkanGlsl);
    auto versionAt = 0;

    while (versionAt < lines.size() && trim(lines[versionAt]).empty())
        ++versionAt;

    if (versionAt == lines.size() || trim(lines[versionAt]) != emittedVersion)
        return failure("eacp: a lowered source has to open with "
                       + std::string {emittedVersion});

    auto result = LoweredGlsl {};
    auto body = std::string {};

    auto section = Section::None;

    for (auto i = 0; i < lines.size(); ++i)
    {
        if (i == versionAt)
            continue;

        section = sectionOf(lines[i], section);

        auto rewrite = rewriteLayout(
            rewriteArrayInitializer(lines[i]), target, section, stage);

        if (rewrite.hasBinding)
            result.bindings.add({rewrite.name, rewrite.binding});

        body += rewrite.line;

        if (i + 1 < lines.size())
            body += '\n';
    }

    if (stage == ShaderStage::Vertex && !renameEntryPoint(body))
        return failure("eacp: a vertex stage has to spell its entry "
                       "\"void main()\"");

    result.source = versionLine(target) + "\n" + stageMacro(stage);

    if (target.isES())
    {
        result.source += "precision highp float;\nprecision highp int;\n";

        // An image is the one type ES declares no default precision for.
        if (containsToken(vulkanGlsl, "image2D"))
            result.source += "precision highp image2D;\n";
    }

    result.source += body;

    if (stage == ShaderStage::Vertex)
        result.source += clipYWrapper;

    return result;
}
} // namespace eacp::GPU
