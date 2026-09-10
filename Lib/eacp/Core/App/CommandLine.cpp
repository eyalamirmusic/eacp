#include "CommandLine.h"
#include "AppEnvironment.h"

#include <concepts>
#include <cstdlib>
#include <type_traits>
#include <variant>

namespace eacp::Apps
{
namespace
{
bool startsSwitch(std::string_view token)
{
    return token.starts_with("--");
}

bool endsSwitches(std::string_view token)
{
    return token == "--";
}

bool isSwitch(std::string_view token)
{
    return startsSwitch(token) && !endsSwitches(token);
}

bool isAsciiUpper(char character)
{
    return character >= 'A' && character <= 'Z';
}

bool isAsciiLowerOrDigit(char character)
{
    return (character >= 'a' && character <= 'z')
           || (character >= '0' && character <= '9');
}

bool startsNewWord(char previous, char character)
{
    return isAsciiUpper(character) && isAsciiLowerOrDigit(previous);
}

std::optional<double> parseWholeDouble(const std::string& text)
{
    if (text.empty())
        return std::nullopt;

    auto* end = static_cast<char*>(nullptr);
    const auto value = std::strtod(text.c_str(), &end);

    if (end != text.c_str() + text.size())
        return std::nullopt;

    return value;
}

template <typename T>
std::optional<T> parseNumber(const std::string& text)
{
    if constexpr (std::same_as<T, double>)
        return parseWholeDouble(text);
    else
        return Strings::tryParse<T>(text);
}

template <typename T>
std::string_view numberTypeName()
{
    if constexpr (std::same_as<T, double>)
        return "double";
    else if constexpr (std::same_as<T, std::int64_t>)
        return "int64";
    else
        return "int";
}

std::string_view articleFor(std::string_view typeName)
{
    const auto vowels = std::string_view {"aeiou"};

    return vowels.find(typeName.front()) != std::string_view::npos ? "an" : "a";
}

std::string joinPath(const std::string& path, std::string_view key)
{
    return path.empty() ? std::string {key} : Strings::concat(path, ".", key);
}

Vector<std::string> withoutExecutablePath(const Vector<std::string>& argv)
{
    auto args = Vector<std::string> {};

    for (auto index = 1; index < argv.size(); ++index)
        args.add(argv[index]);

    return args;
}
} // namespace

std::string Argument::toString(const std::string& fallback) const
{
    return present && !bare ? value : fallback;
}

int Argument::toInt(int fallback) const
{
    return present ? parseNumber<int>(value).value_or(fallback) : fallback;
}

std::int64_t Argument::toInt64(std::int64_t fallback) const
{
    return present ? parseNumber<std::int64_t>(value).value_or(fallback) : fallback;
}

double Argument::toDouble(double fallback) const
{
    return present ? parseWholeDouble(value).value_or(fallback) : fallback;
}

float Argument::toFloat(float fallback) const
{
    return static_cast<float>(toDouble(fallback));
}

bool Argument::toBool(bool fallback) const
{
    if (!present)
        return fallback;

    if (bare)
        return true;

    const auto text = Strings::toLower(value);

    return !(text == "false" || text == "no" || text == "off" || text == "0");
}

std::string Report::toString() const
{
    auto text = std::string {};

    for (const auto& error: errors)
        text += Strings::concat(error, "\n");

    for (const auto& name: unknown)
        text += Strings::concat("unknown option --", name, "\n");

    return text;
}

std::string normalizeOptionName(std::string_view name)
{
    auto stripped = std::string {};
    stripped.reserve(name.size());

    for (auto character: name)
        if (character != '-' && character != '_')
            stripped += character;

    return Strings::toLower(stripped);
}

std::string kebabCase(std::string_view fieldName)
{
    auto spaced = std::string {};

    for (auto index = std::size_t {0}; index < fieldName.size(); ++index)
    {
        if (index > 0 && startsNewWord(fieldName[index - 1], fieldName[index]))
            spaced += '-';

        spaced += fieldName[index];
    }

    return Strings::toLower(spaced);
}

CommandLine::CommandLine()
    : CommandLine(withoutExecutablePath(getAppEnvironment().commandLineArgs))
{
}

CommandLine::CommandLine(const Vector<std::string>& args)
{
    auto parsingSwitches = true;
    auto commandTaken = false;

    for (auto index = 0; index < args.size(); ++index)
    {
        const auto& token = args[index];

        if (!parsingSwitches)
        {
            positionals.add(token);
            continue;
        }

        if (endsSwitches(token))
        {
            parsingSwitches = false;
            continue;
        }

        if (isSwitch(token))
        {
            const auto body = std::string_view {token}.substr(2);
            const auto separator = body.find('=');

            auto option = Option {};
            option.argument.present = true;

            if (separator != std::string_view::npos)
            {
                option.name = std::string {body.substr(0, separator)};
                option.argument.value = std::string {body.substr(separator + 1)};
            }
            else
            {
                option.name = std::string {body};

                if (index + 1 < args.size() && !startsSwitch(args[index + 1]))
                    option.argument.value = args[++index];
                else
                    option.argument.bare = true;
            }

            options.add(option);
            continue;
        }

        if (options.empty() && !commandTaken)
        {
            command = token;
            commandTaken = true;
            continue;
        }

        positionals.add(token);
    }
}

CommandLine CommandLine::fromArgv(int argc, char* argv[])
{
    auto args = Vector<std::string> {};

    for (auto index = 1; index < argc; ++index)
        args.add(argv[index]);

    return CommandLine {args};
}

bool CommandLine::empty() const
{
    return command.empty() && positionals.empty() && options.empty();
}

Argument CommandLine::operator[](std::string_view name) const
{
    const auto normalized = normalizeOptionName(name);
    auto found = Argument {};

    for (const auto& option: options)
        if (normalizeOptionName(option.name) == normalized)
            found = option.argument;

    return found;
}

bool CommandLine::has(std::string_view name) const
{
    return (*this)[name].present;
}

bool CommandLine::getBool(std::string_view name, bool fallback) const
{
    return (*this)[name].toBool(fallback);
}

Vector<Argument> CommandLine::all(std::string_view name) const
{
    const auto normalized = normalizeOptionName(name);
    auto matches = Vector<Argument> {};

    for (const auto& option: options)
        if (normalizeOptionName(option.name) == normalized)
            matches.add(option.argument);

    return matches;
}

namespace Detail
{
CommandLineReflector::CommandLineReflector(const CommandLine& commandLineToUse,
                                           Report& reportToUse)
    : Miro::Reflector(
          Miro::Options {.mode = Miro::Mode::Load, .shape = Miro::Shape::Object})
    , commandLine(commandLineToUse)
    , report(reportToUse)
    , claimed(rootClaimed)
{
}

CommandLineReflector::CommandLineReflector(const CommandLineReflector& parent,
                                           const std::string& pathToUse,
                                           const Vector<Argument>& boundToUse,
                                           const Miro::Options& options)
    : Miro::Reflector(options)
    , commandLine(parent.commandLine)
    , report(parent.report)
    , path(pathToUse)
    , bound(boundToUse)
    , claimed(parent.claimed)
{
}

Miro::ValueKind CommandLineReflector::kind() const
{
    if (bound.empty())
        return Miro::ValueKind::Absent;

    return bound.back().bare ? Miro::ValueKind::Bool : Miro::ValueKind::String;
}

Miro::Reflector& CommandLineReflector::atKey(std::string_view key,
                                             Miro::Options childOptions)
{
    const auto childPath = joinPath(path, key);
    claimed.addIfNotThere(normalizeOptionName(childPath));

    currentChild = new CommandLineReflector {
        *this, childPath, commandLine.all(childPath), childOptions};

    return *currentChild;
}

Miro::Reflector& CommandLineReflector::atIndex(std::size_t index,
                                               Miro::Options childOptions)
{
    auto childBound = Vector<Argument> {};

    if (index < bound.getSize())
        childBound.add(bound[index]);

    currentChild = new CommandLineReflector {*this, path, childBound, childOptions};

    return *currentChild;
}

std::size_t CommandLineReflector::arraySize() const
{
    return bound.getSize();
}

void CommandLineReflector::visit(Miro::PrimitiveRef ref)
{
    if (bound.empty())
        return;

    const auto& argument = bound.back();

    auto read = [this, &argument](auto* target)
    {
        using Value = std::remove_pointer_t<decltype(target)>;

        if constexpr (std::same_as<Value, bool>)
        {
            *target = argument.toBool();
        }
        else if constexpr (std::same_as<Value, std::string>)
        {
            if (argument.bare)
                reportError("expects a value");
            else
                *target = argument.value;
        }
        else
        {
            const auto typeName = numberTypeName<Value>();

            if (const auto parsed = parseNumber<Value>(argument.value))
                *target = *parsed;
            else
                reportError(Strings::concat("'",
                                            argument.value,
                                            "' is not ",
                                            articleFor(typeName),
                                            " ",
                                            typeName));
        }
    };

    std::visit(read, ref.data);
}

void CommandLineReflector::reportError(const std::string& message)
{
    report.errors.add(Strings::concat("--", kebabCase(path), ": ", message));
}

void CommandLineReflector::finish()
{
    for (const auto& option: commandLine.options)
        if (!claimed.contains(normalizeOptionName(option.name)))
            report.unknown.addIfNotThere(option.name);
}

namespace
{
std::string usageLeftColumn(const UsageReflector::Line& line)
{
    auto text = Strings::concat("  --", kebabCase(line.path));

    if (line.type != "bool")
        text += Strings::concat(" <", line.type, ">");

    if (line.repeated)
        text += "...";

    return text;
}

std::string withoutTrailingZeros(const std::string& decimal)
{
    if (decimal.find('.') == std::string::npos)
        return decimal;

    auto end = decimal.find_last_not_of('0');

    if (decimal[end] == '.')
        --end;

    return decimal.substr(0, end + 1);
}

template <typename T>
std::string defaultText(T value)
{
    if (value == T {})
        return {};

    if constexpr (std::same_as<T, double>)
        return withoutTrailingZeros(Strings::toString(value));
    else
        return Strings::toString(value);
}

std::string usageRightColumn(const UsageReflector::Line& line)
{
    if (!line.defaultValue.empty())
        return Strings::concat("default ", line.defaultValue);

    if (line.optional)
        return "optional";

    return {};
}
} // namespace

UsageReflector::UsageReflector(Vector<Line>& linesToUse,
                               const Miro::Options& options)
    : Miro::Reflector(options)
    , lines(linesToUse)
    , optional(options.nullable)
{
}

UsageReflector::UsageReflector(const UsageReflector& parent,
                               const std::string& pathToUse,
                               bool repeatedToUse,
                               const Miro::Options& options)
    : Miro::Reflector(options)
    , lines(parent.lines)
    , path(pathToUse)
    , optional(parent.optional || options.nullable)
    , repeated(repeatedToUse)
{
}

Miro::Reflector& UsageReflector::atKey(std::string_view key,
                                       Miro::Options childOptions)
{
    currentChild =
        new UsageReflector {*this, joinPath(path, key), repeated, childOptions};

    return *currentChild;
}

Miro::Reflector& UsageReflector::atIndex(std::size_t, Miro::Options childOptions)
{
    currentChild = new UsageReflector {*this, path, true, childOptions};

    return *currentChild;
}

void UsageReflector::visit(Miro::PrimitiveRef ref)
{
    auto describe = [this](auto* target)
    {
        using Value = std::remove_pointer_t<decltype(target)>;

        if constexpr (std::same_as<Value, bool>)
            record("bool", {});
        else if constexpr (std::same_as<Value, std::string>)
            record("string", *target);
        else
            record(numberTypeName<Value>(), defaultText(*target));
    };

    std::visit(describe, ref.data);
}

void UsageReflector::visitEnum(Miro::TypeId, const Vector<std::string_view>& names)
{
    auto enumerators = std::string {};

    for (const auto& name: names)
    {
        if (!enumerators.empty())
            enumerators += "|";

        enumerators += name;
    }

    record(enumerators, {});
}

UsageReflector::Line* UsageReflector::findRecordedLine()
{
    for (auto& line: lines)
        if (line.path == path)
            return &line;

    return nullptr;
}

void UsageReflector::record(std::string_view type, const std::string& defaultValue)
{
    const auto suppressesDefault = optional || repeated;

    if (auto* recorded = findRecordedLine())
    {
        if (recorded->defaultValue.empty() && !suppressesDefault)
            recorded->defaultValue = defaultValue;

        return;
    }

    if (!isSchema())
        return;

    lines.add(Line {path,
                    std::string {type},
                    suppressesDefault ? std::string {} : defaultValue,
                    optional,
                    repeated});
}

std::string UsageReflector::render(std::string_view program,
                                   const Vector<Line>& linesToRender)
{
    auto text = program.empty()
                    ? std::string {"Usage: [command] [options]\n"}
                    : Strings::concat("Usage: ", program, " [command] [options]\n");

    auto widest = std::size_t {0};

    for (const auto& line: linesToRender)
        if (const auto width = usageLeftColumn(line).size(); width > widest)
            widest = width;

    const auto rightColumn = widest + 3;

    for (const auto& line: linesToRender)
    {
        const auto left = usageLeftColumn(line);
        const auto right = usageRightColumn(line);

        text += left;

        if (!right.empty())
            text += std::string(rightColumn - left.size(), ' ') + right;

        text += "\n";
    }

    return text;
}
} // namespace Detail
} // namespace eacp::Apps
