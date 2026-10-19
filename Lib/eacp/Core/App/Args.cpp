#include "Args.h"
#include "AppEnvironment.h"

#include "../Utils/Console.h"
#include "../Utils/Environment.h"

#include <algorithm>
#include <locale>
#include <sstream>

namespace eacp::Args
{
namespace
{
constexpr auto helpName = std::string_view {"--help"};
constexpr auto helpShortName = std::string_view {"-h"};
constexpr auto endOfOptions = std::string_view {"--"};

struct HelpRow
{
    std::string left;
    std::string right;
};

Result failure(std::string error)
{
    return {.ok = false, .help = false, .error = std::move(error)};
}

std::string unknownArgument(std::string_view text)
{
    return Strings::concat("unknown argument '", text, "' (try --help)");
}

bool isHelpRequest(std::string_view arg)
{
    return arg == helpName || arg == helpShortName;
}

bool asksForHelp(const Vector<std::string>& args)
{
    for (const auto& arg: args)
    {
        if (arg == endOfOptions)
            return false;

        if (isHelpRequest(arg))
            return true;
    }

    return false;
}

bool looksLikeAnOption(std::string_view arg)
{
    return arg.size() > 1 && arg.front() == '-';
}

struct SplitArgument
{
    std::string_view key;
    std::optional<std::string_view> inlineValue;
};

// Only the long form takes "=": "-o=x" would be ambiguous with a value that
// starts with '='.
SplitArgument splitInlineValue(std::string_view arg)
{
    if (arg.starts_with("--"))
        if (auto equals = arg.find('='); equals != std::string_view::npos)
            return {arg.substr(0, equals), arg.substr(equals + 1)};

    return {arg, std::nullopt};
}

// "(repeatable; one of vst3, au)", or nothing for no notes.
std::string parenthesised(const Vector<std::string>& notes)
{
    auto joined = std::string {};

    for (const auto& note: notes)
    {
        if (!joined.empty())
            joined += "; ";

        joined += note;
    }

    return joined.empty() ? joined : "(" + joined + ")";
}

Vector<std::string> notesFor(const Detail::Option& option)
{
    auto notes = Vector<std::string> {};

    if (option.required)
        notes.add("required");

    if (option.isRepeatable())
        notes.add("repeatable");

    if (auto choices = option.choices(); !choices.empty())
        notes.add("one of " + choices);

    return notes;
}

void appendPart(std::string& text, std::string_view part)
{
    if (part.empty())
        return;

    if (!text.empty())
        text += ' ';

    text += part;
}

constexpr auto helpWidth = std::size_t {80};

// Below this, a description column pushed far right by a long option name
// wraps at this many characters instead, overrunning the width rather than
// becoming one word per line.
constexpr auto minimumWrapRoom = std::size_t {20};

// A word and the run of spaces before it, kept so that text which fits keeps
// its own spacing ("0  success" in an aligned table).
struct Word
{
    std::string_view gap;
    std::string_view text;
};

Vector<Word> splitWords(std::string_view text)
{
    auto words = Vector<Word> {};

    while (true)
    {
        auto start = std::min(text.find_first_not_of(' '), text.size());

        if (start == text.size())
            break;

        auto end = std::min(text.find(' ', start), text.size());
        words.add({text.substr(0, start), text.substr(start, end - start)});
        text.remove_prefix(end);
    }

    return words;
}

// Fills lines of at most `room` characters with the words of `line`, on
// spaces only; a word longer than `room` gets a line of its own.
Vector<std::string> wrapLine(std::string_view line, std::size_t room)
{
    auto lines = Vector<std::string> {};
    auto current = std::string {};

    for (auto word: splitWords(line))
    {
        if (!current.empty()
            && current.size() + word.gap.size() + word.text.size() > room)
        {
            lines.add(current);
            current.clear();
        }

        if (!current.empty())
            current += word.gap;

        current += word.text;
    }

    if (!current.empty() || lines.empty())
        lines.add(current);

    return lines;
}

// Wraps each line of `text` on its own, so explicit newlines and blank lines
// stand. A line's leading spaces are kept and repeated on its continuations,
// which keeps an indented table entry together.
std::string wrapText(std::string_view text, std::size_t indent)
{
    auto room = std::max(minimumWrapRoom, helpWidth - std::min(indent, helpWidth));
    auto wrapped = std::string {};
    auto first = true;

    while (true)
    {
        auto end = text.find('\n');
        auto line = text.substr(0, end);
        auto lead = std::min(line.find_first_not_of(' '), line.size());
        auto margin = std::string(lead, ' ');
        auto lineRoom = std::max(minimumWrapRoom, room - std::min(lead, room));

        for (const auto& piece: wrapLine(line.substr(lead), lineRoom))
        {
            if (!first)
                wrapped += "\n" + std::string(indent, ' ');

            first = false;
            wrapped += piece.empty() ? piece : margin + piece;
        }

        if (end == std::string_view::npos)
            break;

        text.remove_prefix(end + 1);
    }

    return wrapped;
}

void appendRows(std::string& text,
                std::string_view heading,
                const Vector<HelpRow>& rows,
                int width)
{
    if (rows.empty())
        return;

    text += Strings::concat("\n\n", heading, ":");

    for (const auto& row: rows)
    {
        auto line = "  " + row.left;

        if (!row.right.empty())
        {
            line.resize(static_cast<std::size_t>(width), ' ');
            line += wrapText(row.right, static_cast<std::size_t>(width));
        }

        text += "\n" + line;
    }
}
} // namespace

bool Detail::isWholeNumber(std::string_view text)
{
    return Strings::tryParse<long long>(text).has_value()
           || Strings::tryParse<unsigned long long>(text).has_value();
}

std::string Detail::formatFloating(double value)
{
    auto stream = std::ostringstream {};
    stream.imbue(std::locale::classic());
    stream << value;
    return stream.str();
}

Parser::Parser(std::string nameToUse, std::string descriptionToUse)
    : name(std::move(nameToUse))
    , description(std::move(descriptionToUse))
{
}

OptionBuilder<bool>
    Parser::flag(std::string_view name, bool& target, std::string_view description)
{
    return flag(name, {}, target, description);
}

OptionBuilder<bool> Parser::flag(std::string_view name,
                                 std::string_view shortName,
                                 bool& target,
                                 std::string_view description)
{
    auto builder = option(name, shortName, target, description);
    options.back()->takesValue = false;
    return builder;
}

void Parser::epilogue(std::string text)
{
    epilogueText = std::move(text);
}

void Parser::describe(Detail::Option& option,
                      std::string_view nameToUse,
                      std::string_view shortName,
                      std::string_view descriptionToUse)
{
    option.name = std::string {nameToUse};
    option.shortName = std::string {shortName};
    option.description = std::string {descriptionToUse};
}

void Parser::saveDefaults()
{
    if (defaultsSaved)
        return;

    defaultsSaved = true;

    for (auto& option: options)
        option->savedDefault = option->currentDefaultText();

    for (auto& positional: positionals)
        positional->savedDefault = positional->currentDefaultText();
}

std::optional<std::string> Parser::applyEnvironment()
{
    for (auto& option: options)
    {
        if (option->environmentVariable.empty())
            continue;

        auto value = getEnv(option->environmentVariable);

        if (!value || value->empty())
            continue;

        auto subject = Strings::concat(
            option->environmentVariable, " (for ", option->name, ")");

        if (auto error = option->assign(*value, subject))
            return error;
    }

    return std::nullopt;
}

Detail::Option* Parser::findOption(std::string_view text) const
{
    for (const auto& option: options)
        if (option->name == text
            || (!option->shortName.empty() && option->shortName == text))
            return option.get();

    return nullptr;
}

bool Parser::isOptionName(std::string_view text) const
{
    return text.starts_with("--") || isHelpRequest(text)
           || findOption(text) != nullptr;
}

Result Parser::parse(const Vector<std::string>& args)
{
    saveDefaults();

    if (asksForHelp(args))
        return {.ok = false, .help = true, .error = {}};

    for (auto& option: options)
    {
        option->startNewSource();
        option->given = false;
    }

    for (auto& positional: positionals)
    {
        positional->startNewSource();
        positional->given = false;
    }

    if (auto error = applyEnvironment())
        return failure(std::move(*error));

    for (auto& option: options)
        option->startNewSource();

    auto nextPositional = 0;
    auto onlyPositionals = false;

    for (auto index = 0; index < args.size(); ++index)
    {
        const auto& arg = args[index];

        if (!onlyPositionals && arg == endOfOptions)
        {
            onlyPositionals = true;
            continue;
        }

        if (!onlyPositionals && looksLikeAnOption(arg))
        {
            auto [key, inlineValue] = splitInlineValue(arg);
            auto* option = findOption(key);

            if (option == nullptr)
                return failure(unknownArgument(key));

            if (!option->takesValue)
            {
                if (inlineValue)
                    return failure(Strings::concat(key, " takes no value"));

                if (auto error = option->assign(option->flagValue, key))
                    return failure(std::move(*error));

                continue;
            }

            auto value = inlineValue;

            if (!value && index + 1 < args.size() && !isOptionName(args[index + 1]))
                value = args[++index];

            if (!value)
                return failure(Strings::concat(key, " needs a value"));

            if (auto error = option->assign(*value, key))
                return failure(std::move(*error));

            continue;
        }

        if (nextPositional >= positionals.size())
            return failure(unknownArgument(arg));

        auto& positional = *positionals[nextPositional];

        if (auto error = positional.assign(arg, positional.name))
            return failure(std::move(*error));

        if (!positional.isRepeatable())
            ++nextPositional;
    }

    if (auto error = missingRequired())
        return failure(std::move(*error));

    return {.ok = true, .help = false, .error = {}};
}

std::optional<std::string> Parser::missingRequired() const
{
    for (const auto& rows: {&options, &positionals})
        for (const auto& option: *rows)
            if (option->required && !option->given)
                return Strings::concat(option->name, " is required (try --help)");

    return std::nullopt;
}

std::string Parser::hintOf(const Detail::Option& option) const
{
    return option.hintOverride.empty() ? option.hint() : option.hintOverride;
}

Result Parser::parse()
{
    const auto& stored = Apps::commandLineArgs();
    auto args = Vector<std::string> {};

    for (auto index = 1; index < static_cast<int>(stored.size()); ++index)
        args.add(stored[index]);

    return parse(args);
}

Result Parser::parse(int argc, const char* const* argv)
{
    auto args = Vector<std::string> {};

    for (auto index = 1; index < argc; ++index)
        args.add(argv[index] != nullptr ? std::string {argv[index]}
                                        : std::string {});

    return parse(args);
}

std::string Parser::defaultTextOf(const Detail::Option& option) const
{
    return option.savedDefault.value_or(option.currentDefaultText());
}

std::string Parser::usageLine() const
{
    auto line = Strings::concat("Usage: ", name);

    for (const auto& option: options)
        if (option->required)
            line += Strings::concat(" ",
                                    option->name,
                                    option->takesValue ? " " + hintOf(*option)
                                                       : std::string {});

    line += " [options]";

    for (const auto& positional: positionals)
    {
        auto spelled = positional->name + (positional->isRepeatable() ? "..." : "");
        line += positional->required ? " " + spelled : " [" + spelled + "]";
    }

    return line;
}

std::string Parser::defaultShownFor(const Detail::Option& option) const
{
    if (option.defaultOverride)
        return *option.defaultOverride;

    // A flag's default is its absence, which says nothing worth printing, and a
    // negated one's "true" would read as the flag being on.
    if (!option.takesValue)
        return {};

    return defaultTextOf(option);
}

std::string Parser::helpText() const
{
    auto argumentRows = Vector<HelpRow> {};
    auto optionRows = Vector<HelpRow> {};

    for (const auto& positional: positionals)
    {
        auto value = defaultShownFor(*positional);

        if (positional->description.empty() && value.empty())
            continue;

        auto right = positional->description;
        appendPart(right, parenthesised(notesFor(*positional)));

        if (!value.empty())
            appendPart(right, Strings::concat("[default: ", value, "]"));

        argumentRows.add({positional->name, right});
    }

    for (const auto& option: options)
    {
        auto left = option->shortName.empty() ? std::string {"    "}
                                              : option->shortName + ", ";
        left += option->name;

        if (option->takesValue)
            left += " " + hintOf(*option);

        auto right = option->description;
        appendPart(right, parenthesised(notesFor(*option)));

        if (!option->environmentVariable.empty())
            appendPart(right,
                       Strings::concat("[env: ", option->environmentVariable, "]"));

        if (auto value = defaultShownFor(*option); !value.empty())
            appendPart(right, Strings::concat("[default: ", value, "]"));

        optionRows.add({left, right});
    }

    optionRows.add(
        {Strings::concat(helpShortName, ", ", helpName), "show this help and exit"});

    auto width = 0;

    for (const auto& rows: {&argumentRows, &optionRows})
        for (const auto& row: *rows)
            width = std::max(width, static_cast<int>(row.left.size()) + 4);

    auto text = usageLine();

    if (!description.empty())
        text += "\n\n" + wrapText(description, 0);

    appendRows(text, "Arguments", argumentRows, width);
    appendRows(text, "Options", optionRows, width);

    if (!epilogueText.empty())
        text += "\n\n" + wrapText(epilogueText, 0);

    return text;
}

std::optional<int> Parser::exitCodeFor(const Result& result) const
{
    if (result.help)
    {
        Console::print(helpText());
        return 0;
    }

    if (!result.ok)
        return fail(result.error);

    return std::nullopt;
}

int Parser::fail(std::string_view error) const
{
    Console::printError(Strings::concat(name, ": ", error));
    return badArguments;
}
} // namespace eacp::Args
