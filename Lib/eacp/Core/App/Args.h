#pragma once

#include "../Utils/Containers.h"
#include "../Utils/FilePath.h"
#include "../Utils/Strings.h"

#include <Miro/Reflection/ReflectEnum.h>

#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// A declarative command-line parser that binds each option to a variable the
// caller already has, so there is no parallel options struct to copy out of:
//
//   auto cli = Args::Parser {"Scanner", "Describes every plugin."};
//   cli.option("--output", "-o", options.cachePath, "the database to write");
//   cli.option("--jobs", options.jobs, "parallel workers").atLeast(1);
//   cli.flag("--force", options.force, "rescan everything");
//   cli.positional(bundles, "bundle");
//
//   if (auto code = cli.exitCodeFor(cli.parse()))
//       return *code;
//
// parse() reads the command line runConsole stored; parse(argc, argv) and
// parse(args) take one explicitly.
//
// The bound variables' values when parse() is first called are the defaults the
// help text shows, so they never drift from the code.
namespace eacp::Args
{
// The exit code for arguments that did not parse: the usual one for a usage
// error (getopt tools, Python's argparse).
constexpr int badArguments = 2;

// ok: the program should go on. help: -h/--help was asked for. Otherwise
// error says what was wrong, e.g. "--jobs needs a whole number, not 'abc'".
struct Result
{
    bool ok = false;
    bool help = false;
    std::string error;
};

namespace Detail
{
// What a bound variable holds: a Vector repeats, an optional is set only when
// a value is given, and either way Element is the type each value parses as.
template <typename T>
struct TargetTraits
{
    static constexpr bool isVector = false;
    static constexpr bool isOptional = false;
    using Element = T;
};

template <typename T>
struct TargetTraits<std::optional<T>>
{
    static constexpr bool isVector = false;
    static constexpr bool isOptional = true;
    using Element = T;
};

template <typename T, typename Allocator>
struct TargetTraits<EA::Vector<T, Allocator>>
{
    static constexpr bool isVector = true;
    static constexpr bool isOptional = false;
    using Element = T;
};

template <typename T>
constexpr bool alwaysFalse = false;

template <typename T>
std::string joinedEnumNames()
{
    auto joined = std::string {};

    for (auto name: Miro::enumNames<T>())
    {
        if (!joined.empty())
            joined += ", ";

        joined += name;
    }

    return joined;
}

template <typename T>
std::optional<T> parseEnum(std::string_view text)
{
    if (auto exact = Miro::enumFromString<T>(text))
        return exact;

    for (auto name: Miro::enumNames<T>())
        if (Strings::equalsCaseInsensitive(name, text))
            return Miro::enumFromString<T>(name);

    return std::nullopt;
}

// The customisation point every option and positional goes through: text to a
// T, or nullopt when the text is not one.
template <typename T>
std::optional<T> parseValue(std::string_view text)
{
    if constexpr (std::is_same_v<T, bool>)
        return Strings::tryParseBool(text);
    else if constexpr (std::is_enum_v<T>)
        return parseEnum<T>(text);
    else if constexpr (std::is_arithmetic_v<T>)
        return Strings::tryParse<T>(text);
    else if constexpr (std::is_same_v<T, std::string>)
        return std::string {text};
    else if constexpr (std::is_same_v<T, FilePath>)
        return FilePath {text};
    else
        static_assert(alwaysFalse<T>,
                      "Args handles integers, float, double, bool, enums, "
                      "std::string, FilePath and Vectors of those");
}

bool isWholeNumber(std::string_view text);

// The shortest plain spelling for the help text, independent of the locale.
std::string formatFloating(double value);

// What parseValue<T> wanted, phrased to follow the option's name:
// "--jobs needs a whole number".
template <typename T>
std::string expectation(std::string_view text)
{
    if constexpr (std::is_same_v<T, bool>)
        return "needs true or false";
    else if constexpr (std::is_enum_v<T>)
        return "must be one of " + joinedEnumNames<T>();
    else if constexpr (std::is_integral_v<T>)
    {
        if (isWholeNumber(text))
            return Strings::concat("must be from ",
                                   std::numeric_limits<T>::lowest(),
                                   " to ",
                                   std::numeric_limits<T>::max());

        return "needs a whole number";
    }
    else if constexpr (std::is_floating_point_v<T>)
        return "needs a number";
    else
        return "has an unusable value";
}

template <typename T>
std::string valueText(const T& value)
{
    if constexpr (std::is_same_v<T, bool>)
        return value ? "true" : "false";
    else if constexpr (std::is_enum_v<T>)
        return std::string {Miro::enumToString(value)};
    else if constexpr (std::is_floating_point_v<T>)
        return formatFloating(static_cast<double>(value));
    else if constexpr (std::is_arithmetic_v<T>)
        return Strings::toString(value);
    else if constexpr (std::is_same_v<T, FilePath>)
        return value.str();
    else
        return value;
}

// The bound variable's value as the help text shows it, or empty when there is
// nothing worth showing: false, zero, an empty string or Vector. An enum is
// always shown, since its zero is a real choice. An optional has none: its
// default is "not given".
template <typename T>
std::string defaultText(const T& value)
{
    if constexpr (TargetTraits<T>::isOptional)
        return {};
    else if constexpr (TargetTraits<T>::isVector)
    {
        auto joined = std::string {};

        for (const auto& element: value)
        {
            if (!joined.empty())
                joined += ", ";

            joined += valueText(element);
        }

        return joined;
    }
    else if constexpr (std::is_enum_v<T>)
        return valueText(value);
    else if constexpr (std::is_arithmetic_v<T>)
        return value == T {} ? std::string {} : valueText(value);
    else
        return valueText(value);
}

template <typename T>
std::string valueHint()
{
    if constexpr (std::is_same_v<T, bool>)
        return "<bool>";
    else if constexpr (std::is_enum_v<T>)
        return "<name>";
    else if constexpr (std::is_integral_v<T>)
        return "<n>";
    else if constexpr (std::is_floating_point_v<T>)
        return "<number>";
    else if constexpr (std::is_same_v<T, FilePath>)
        return "<path>";
    else
        return "<value>";
}

// One option or positional, as the parser stores it. The parsing and the help
// text work through this; TypedOption below knows the bound variable's type.
struct Option
{
    virtual ~Option() = default;

    // Parses `text` into the bound variable after every check has passed, or
    // returns the error, worded with `subject` ("--jobs", or "JOBS (for
    // --jobs)" when the text came from the environment).
    virtual std::optional<std::string> assign(std::string_view text,
                                              std::string_view subject) = 0;

    // A repeatable option's first assignment from a new source (the
    // environment, then argv) replaces what the Vector held; later ones append.
    virtual void startNewSource() = 0;

    virtual std::string currentDefaultText() const = 0;
    virtual std::string hint() const = 0;
    virtual std::string choices() const = 0;
    virtual bool isRepeatable() const = 0;

    std::string name;
    std::string shortName;
    std::string description;
    std::string environmentVariable;
    bool takesValue = true;

    // What a flag (takesValue false) stores when it is present: "false" once
    // negated().
    std::string flagValue = "true";

    // The builder's hint() and defaultText(), which win over the generated
    // ones when set.
    std::string hintOverride;
    std::optional<std::string> defaultOverride;

    // required(): parse() fails unless the environment or the command line
    // gave a value, which `given` records for the parse under way.
    bool required = false;
    bool given = false;

    // currentDefaultText() as it was when parse() first ran, so a help text
    // printed after parsing still shows the defaults rather than what was
    // passed.
    std::optional<std::string> savedDefault;
};

template <typename T>
struct TypedOption : Option
{
    using Value = typename TargetTraits<T>::Element;
    using Rule = std::function<std::optional<std::string>(const Value&)>;

    explicit TypedOption(T& targetToUse)
        : target(targetToUse)
    {
    }

    // An integer whose bounds are checked before it is narrowed to Value, so a
    // uint16_t with atMost(1000) reports "must be at most 1000" for 70000
    // rather than the type's own range. std::cmp_less refuses the character
    // types, which are left to the plain path.
    static constexpr bool checksBoundsWide =
        std::is_integral_v<Value> && !std::is_same_v<Value, bool>
        && !std::is_same_v<Value, char> && !std::is_same_v<Value, wchar_t>
        && !std::is_same_v<Value, char8_t> && !std::is_same_v<Value, char16_t>
        && !std::is_same_v<Value, char32_t>;

    std::optional<std::string> assign(std::string_view text,
                                      std::string_view subject) override
    {
        if constexpr (checksBoundsWide)
            if (auto problem = wideBoundsProblem(text))
                return describeProblem(subject, *problem, text);

        auto value = parseValue<Value>(text);

        if (!allowed.empty())
            value = pickAllowed(value, text);

        if (!value)
        {
            auto problem = allowed.empty() ? expectation<Value>(text)
                                           : "must be one of " + choices();
            return describeProblem(subject, problem, text);
        }

        if constexpr (!checksBoundsWide
                      && std::is_arithmetic_v<Value> && !std::is_same_v<Value, bool>)
            if (auto problem = boundsProblem(*value))
                return describeProblem(subject, *problem, text);

        for (const auto& rule: rules)
            if (auto problem = rule(*value))
                return describeProblem(subject, *problem, text);

        store(std::move(*value));
        given = true;
        return std::nullopt;
    }

    // The atLeast()/atMost() phrase for `value`, compared as the type it came
    // in as, so a value wider than Value is still compared exactly.
    template <typename Number>
    std::optional<std::string> boundsProblem(Number value) const
    {
        if constexpr (checksBoundsWide)
        {
            if (minimum && std::cmp_less(value, *minimum))
                return "must be at least " + valueText(*minimum);

            if (maximum && std::cmp_greater(value, *maximum))
                return "must be at most " + valueText(*maximum);
        }
        else
        {
            if (minimum && value < *minimum)
                return "must be at least " + valueText(*minimum);

            if (maximum && value > *maximum)
                return "must be at most " + valueText(*maximum);
        }

        return std::nullopt;
    }

    // Reads `text` as the widest integer that holds it and checks the bounds
    // there. Text that is no whole number at all is left for parseValue to
    // report.
    std::optional<std::string> wideBoundsProblem(std::string_view text) const
    {
        if (auto wide = Strings::tryParse<long long>(text))
            return boundsProblem(*wide);

        if (auto wide = Strings::tryParse<unsigned long long>(text))
            return boundsProblem(*wide);

        return std::nullopt;
    }

    void startNewSource() override { appending = false; }

    std::string currentDefaultText() const override { return defaultText(target); }
    std::string hint() const override { return valueHint<Value>(); }

    std::string choices() const override
    {
        if (!allowed.empty())
        {
            auto joined = std::string {};

            for (const auto& choice: allowed)
            {
                if (!joined.empty())
                    joined += ", ";

                joined += valueText<Value>(choice);
            }

            return joined;
        }

        if constexpr (std::is_enum_v<Value>)
            return joinedEnumNames<Value>();
        else
            return {};
    }

    // The allowed value `text` names, or nullopt. An enum has already been
    // parsed, in any case; a string is matched here in any case and takes the
    // allowed spelling.
    std::optional<Value> pickAllowed(const std::optional<Value>& parsed,
                                     std::string_view text) const
    {
        if constexpr (std::is_enum_v<Value>)
        {
            if (parsed && allowed.contains(*parsed))
                return parsed;
        }
        else if constexpr (std::is_same_v<Value, std::string>)
        {
            for (const auto& choice: allowed)
                if (Strings::equalsCaseInsensitive(choice, text))
                    return choice;
        }

        return std::nullopt;
    }

    bool isRepeatable() const override { return TargetTraits<T>::isVector; }

    static std::string describeProblem(std::string_view subject,
                                       std::string_view problem,
                                       std::string_view text)
    {
        return Strings::concat(subject, " ", problem, ", not '", text, "'");
    }

    void store(Value value)
    {
        if constexpr (TargetTraits<T>::isVector)
        {
            if (!appending)
                target.clear();

            appending = true;

            if (!unique || !target.contains(value))
                target.add(std::move(value));
        }
        else
            target = std::move(value);
    }

    T& target;
    Vector<Rule> rules;
    Vector<Value> allowed;
    std::optional<Value> minimum;
    std::optional<Value> maximum;
    bool unique = false;
    bool appending = false;
};
} // namespace Detail

// What option() and flag() return, to refine the option just declared. It
// refers to the parser's own record of it, so use it in the same statement.
template <typename T>
class OptionBuilder
{
public:
    using Value = typename Detail::TargetTraits<T>::Element;

    explicit OptionBuilder(Detail::TypedOption<T>& optionToUse)
        : option(optionToUse)
    {
    }

    // A rule every value must pass, for a Vector each element. It returns
    // nullopt to accept, or a phrase to follow the option's name: a rule
    // answering "must be even" for 3 reports "--jobs must be even, not '3'".
    OptionBuilder check(std::function<std::optional<std::string>(const Value&)> rule)
    {
        option.rules.add(std::move(rule));
        return *this;
    }

    // Bounds on a number, checked before check()'s rules. An integer is
    // compared at full width before it is narrowed, so a uint16_t port with
    // atMost(1024) reports "--port must be at most 1024, not '70000'" rather
    // than the type's own range, which is reported only when no bound already
    // turned the value away.
    OptionBuilder atLeast(Value minimum)
        requires(std::is_arithmetic_v<Value> && !std::is_same_v<Value, bool>)
    {
        option.minimum = minimum;
        return *this;
    }

    OptionBuilder atMost(Value maximum)
        requires(std::is_arithmetic_v<Value> && !std::is_same_v<Value, bool>)
    {
        option.maximum = maximum;
        return *this;
    }

    // parse() fails with "--format is required (try --help)" unless the
    // command line or the environment gave a value. The help marks it, and the
    // usage line lists it before [options] (a positional without brackets).
    OptionBuilder required()
    {
        option.required = true;
        return *this;
    }

    // For a Vector: a value the Vector already holds is skipped, so
    // "--folder a --folder a" gives one entry. Without it every value is kept.
    OptionBuilder unique()
        requires(Detail::TargetTraits<T>::isVector)
    {
        option.unique = true;
        return *this;
    }

    // For a flag(): being present sets the bool to false rather than true, as
    // "--no-default-folders" does to a field that defaults to true. An
    // environment variable from fromEnv() still holds the bool's own value.
    OptionBuilder negated()
        requires(std::is_same_v<T, bool>)
    {
        option.flagValue = "false";
        return *this;
    }

    // Reads the variable `name` at parse time when it is set and not empty,
    // parsed and checked like a command-line value, and lets the command line
    // override it. For a flag the variable holds true/false, yes/no, on/off or
    // 1/0.
    OptionBuilder fromEnv(std::string name)
    {
        option.environmentVariable = std::move(name);
        return *this;
    }

    // Accepts only these values, matched in any case (a string takes the
    // spelling given here), and lists only them in the help. Anything else,
    // unparseable or merely not allowed, reports
    // "--format must be one of vst3, au, not 'clap'".
    OptionBuilder choices(Vector<Value> allowed)
        requires(std::is_enum_v<Value> || std::is_same_v<Value, std::string>)
    {
        option.allowed = std::move(allowed);
        return *this;
    }

    // The value's placeholder in the help, such as "<file>" or "<ms>", in place
    // of the one the type gives.
    OptionBuilder hint(std::string_view text)
    {
        option.hintOverride = std::string {text};
        return *this;
    }

    // What the help shows as "[default: ...]", in place of the bound value,
    // and shown even when that value is empty or zero: "the core count".
    OptionBuilder defaultText(std::string_view text)
    {
        option.defaultOverride = std::string {text};
        return *this;
    }

private:
    Detail::TypedOption<T>& option;
};

// Accepts --name value, --name=value, -short value, -h/--help, and "--" to make
// everything after it positional. An option whose bound variable is a Vector
// may repeat, each occurrence adding one element (one the Vector already holds
// is skipped only with unique()); the first replaces whatever the Vector held
// as its default. A value is the next argument unless that is
// missing or is itself an option ("--" anything, or a declared name), so
// "--offset -5" works. Parsing stops at the first error.
class Parser
{
public:
    explicit Parser(std::string nameToUse, std::string descriptionToUse = {});

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    // Binds `--name value` to `target`. T is an integer, float, double, bool
    // (which then takes a value: --name=false), an enum reflected through Miro,
    // std::string, FilePath, or a Vector or std::optional of one of those. An
    // optional is set only when the command line or the environment gives a
    // value, so the caller can tell "given" from "left alone".
    template <typename T>
    OptionBuilder<T>
        option(std::string_view name, T& target, std::string_view description)
    {
        return option(name, {}, target, description);
    }

    // The same with a short alias, such as "-o".
    template <typename T>
    OptionBuilder<T> option(std::string_view name,
                            std::string_view shortName,
                            T& target,
                            std::string_view description)
    {
        auto& added = options.createDerived<Detail::TypedOption<T>>(target);
        describe(added, name, shortName, description);
        return OptionBuilder<T> {added};
    }

    // A switch with no value: present sets `target` to true.
    OptionBuilder<bool>
        flag(std::string_view name, bool& target, std::string_view description);
    OptionBuilder<bool> flag(std::string_view name,
                             std::string_view shortName,
                             bool& target,
                             std::string_view description);

    // Binds the arguments that are not options, in order. A Vector takes every
    // one that is left, so it goes last; anything else takes one. An argument
    // with nowhere to go is an error. The description, when given, lists the
    // positional in the help text. The builder's check(), atLeast(), atMost()
    // and choices() apply to it as to an option.
    template <typename T>
    OptionBuilder<T> positional(T& target,
                                std::string_view name,
                                std::string_view description = {})
    {
        auto& added = positionals.createDerived<Detail::TypedOption<T>>(target);
        describe(added, name, {}, description);
        return OptionBuilder<T> {added};
    }

    // Free text printed after the options, such as the exit codes.
    void epilogue(std::string text);

    // The command line Apps::getAppEnvironment() stored, without its program
    // name: for a program running under Apps::runConsole, or after
    // Apps::setCommandLineArgs, so argc/argv need not be passed down to it.
    Result parse();

    // `args` without the program name.
    Result parse(const Vector<std::string>& args);

    // main()'s own, argv[0] included. A null entry is read as empty.
    Result parse(int argc, const char* const* argv);

    std::string helpText() const;

    // nullopt when the program should go on. Otherwise prints the help to
    // stdout and returns 0, or prints "<name>: <error>" to stderr and returns
    // badArguments.
    std::optional<int> exitCodeFor(const Result& result) const;

    // Reports a problem found after parsing ("--serve and --connect cannot be
    // combined") the way exitCodeFor reports a parse error: prints
    // "<name>: <error>" to stderr and returns badArguments, for
    // `return cli.fail("...");`.
    int fail(std::string_view error) const;

private:
    void describe(Detail::Option& option,
                  std::string_view name,
                  std::string_view shortName,
                  std::string_view description);

    void saveDefaults();
    std::optional<std::string> applyEnvironment();
    Detail::Option* findOption(std::string_view name) const;
    bool isOptionName(std::string_view text) const;

    std::string usageLine() const;
    std::string defaultTextOf(const Detail::Option& option) const;
    std::string defaultShownFor(const Detail::Option& option) const;
    std::string hintOf(const Detail::Option& option) const;
    std::optional<std::string> missingRequired() const;

    std::string name;
    std::string description;
    std::string epilogueText;
    OwnedVector<Detail::Option> options;
    OwnedVector<Detail::Option> positionals;
    bool defaultsSaved = false;
};
} // namespace eacp::Args
