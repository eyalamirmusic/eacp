#pragma once

#include "../Utils/Common.h"

#include <Miro/Reflect.h>

// argv, parsed once:
//
//   tool build --port 8080 --output-dir=out --verbose --include a --include b -- x
//
//   command      "build"        the first bare word, before any switch
//   options      port=8080, outputDir=out, verbose (bare), include=[a, b]
//   positionals  ["x"]          bare words after the command, and all after "--"
//
// Two ways to read it. Ad hoc, Tamber-style:
//
//   const auto cl = Apps::CommandLine {};        // Apps::run<T>(argc, argv)
//   auto port = cl["port"].toInt(8080);          // captured argv already
//   auto headless = cl.getBool("headless");
//
// Or as a struct, which is the point of this module: declare the options as
// fields with their defaults, list them in MIRO_REFLECT, and the command line
// fills them by name.
//
//   struct Options
//   {
//       std::string input;
//       int port = 8080;
//       bool verbose = false;
//       std::optional<std::string> logFile;
//       Vector<std::string> include;
//       Mode mode = Mode::fast;              // enum class: --mode slow
//       Server server;                       // nested: --server.port 9000
//
//       MIRO_REFLECT(input, port, verbose, logFile, include, mode, server)
//   };
//
//   auto options = Options {};
//   if (auto report = cl.fill(options); !report.ok())
//       std::cerr << report.toString() << Apps::CommandLine::usage<Options>();
//
// Names match case-insensitively with '-' and '_' ignored, so --output-dir,
// --outputDir and --OUTPUT_DIR all reach `outputDir`. A field type decides how
// its text is read: bool takes a bare switch as yes and "false/no/off/0" as
// no; numbers reject trailing junk; an enum takes an enumerator name; an
// optional stays disengaged when the switch is absent; a Vector takes one
// element per repetition of the switch. Anything missing keeps the field's
// default, so a filled struct is always well-formed and the Report says what
// was wrong rather than an exception.

namespace eacp::Apps
{
// One option value. Every conversion takes a fallback, so reading a switch
// nobody passed is the caller's default rather than an error.
struct Argument
{
    bool empty() const { return !present; }
    explicit operator bool() const { return present; }

    std::string toString(const std::string& fallback = {}) const;
    int toInt(int fallback = 0) const;
    std::int64_t toInt64(std::int64_t fallback = 0) const;
    double toDouble(double fallback = 0.0) const;
    float toFloat(float fallback = 0.f) const;

    // A bare switch (`--verbose`) is yes; a value (`--verbose false`) says
    // which; `fallback` answers when the switch is absent altogether.
    bool toBool(bool fallback = false) const;

    // Any type the struct fill understands: numbers, strings, enums by name.
    template <typename T>
    T as(const T& fallback = {}) const;

    bool present = false;
    bool bare = false;
    std::string value;
};

// One switch as it appeared on the command line. Kept in argv order and
// never merged, so `--include a --include b` is two entries.
struct Option
{
    std::string name;
    Argument argument;
};

// What a fill found wrong. A bad value leaves its field at the default; an
// unknown switch is reported but harmless.
struct Report
{
    bool ok() const { return errors.empty() && unknown.empty(); }
    std::string toString() const;

    Vector<std::string> errors;
    Vector<std::string> unknown;
};

struct CommandLine
{
    // main()'s argv as captured by Apps::run<T>(argc, argv) or
    // setCommandLineArgs(), minus the executable path.
    CommandLine();

    // `args` is argv without argv[0]; the constructor for tests and for
    // command lines that arrive some other way (a second instance's, say).
    explicit CommandLine(const Vector<std::string>& args);

    static CommandLine fromArgv(int argc, char* argv[]);

    bool empty() const;

    // Lookup by name, on the same matching rule the struct fill uses. When a
    // switch was repeated, the last occurrence wins here; all() has them all.
    Argument operator[](std::string_view name) const;
    bool has(std::string_view name) const;
    bool getBool(std::string_view name, bool fallback = false) const;
    Vector<Argument> all(std::string_view name) const;

    // Fills every MIRO_REFLECT field of `value` whose name matches a switch,
    // leaving the rest at whatever they already hold. Never throws.
    template <typename T>
    Report fill(T& value) const;

    // A default-constructed T, filled. The Report is available through fill()
    // when the caller wants to know about it.
    template <typename T>
    T get() const;

    // A usage block derived from T alone: one line per field with its type
    // and, when it is not the type's zero, its default:
    //
    //   Usage: tool [command] [options]
    //     --input <string>
    //     --port <int>            default 8080
    //     --verbose
    //     --log-file <string>     optional
    //     --include <string>...
    //     --mode <fast|slow>      default fast
    //     --server.port <int>     default 80
    //
    // Field names are printed kebab-case, the spelling the matching rule
    // accepts alongside every other.
    template <typename T>
    static std::string usage(std::string_view program = {});

    std::string command;
    Vector<std::string> positionals;
    Vector<Option> options;
};

// The matching rule, exposed so tools listing options print the same names
// the parser accepts.
std::string normalizeOptionName(std::string_view name);
std::string kebabCase(std::string_view fieldName);

namespace Detail
{
// A Miro::Reflector in Load mode over a CommandLine. The root is the struct;
// atKey spawns a child bound to `parent.child` and every switch of that name;
// atIndex on an Array child binds the i-th repetition. visit() parses the
// bound text into the primitive it is handed, so bool/int/double/string/enum
// fields all read the way `Argument` does, and Miro's own optional, Vector,
// enum and nested-struct dispatch does the rest.
class CommandLineReflector final : public Miro::Reflector
{
public:
    CommandLineReflector(const CommandLine& commandLine, Report& report);

    void visit(Miro::PrimitiveRef ref) override;
    void writeNull() override {}
    Miro::ValueKind kind() const override;

    Reflector& atKey(std::string_view key, Miro::Options childOptions) override;
    Reflector& atIndex(std::size_t index, Miro::Options childOptions) override;

    std::size_t arraySize() const override;

    void requirePolymorphicSupport(std::string_view) override {}

    // After the walk: every switch no field claimed goes to report.unknown.
    void finish();

private:
    CommandLineReflector(const CommandLineReflector& parent,
                         const std::string& pathToUse,
                         const Vector<Argument>& boundToUse,
                         const Miro::Options& options);

    void reportError(const std::string& message);

    const CommandLine& commandLine;
    Report& report;
    std::string path;
    Vector<Argument> bound;
    Vector<std::string> rootClaimed;
    Vector<std::string>& claimed;
    OwningPointer<CommandLineReflector> currentChild;
};

// The Save-mode walk behind CommandLine::usage<T>(): records each leaf's
// path, primitive type, enumerator names, whether it is optional or repeated,
// and the default text it sees in a default-constructed T.
class UsageReflector final : public Miro::Reflector
{
public:
    struct Line
    {
        std::string path;
        std::string type;
        std::string defaultValue;
        bool optional = false;
        bool repeated = false;
    };

    UsageReflector(Vector<Line>& lines, const Miro::Options& options);

    void visit(Miro::PrimitiveRef ref) override;
    void visitEnum(Miro::TypeId id, const Vector<std::string_view>& names) override;
    void writeNull() override {}
    Miro::ValueKind kind() const override { return Miro::ValueKind::Absent; }

    Reflector& atKey(std::string_view key, Miro::Options childOptions) override;
    Reflector& atIndex(std::size_t index, Miro::Options childOptions) override;

    void requirePolymorphicSupport(std::string_view) override {}

    static std::string render(std::string_view program, const Vector<Line>& lines);

private:
    UsageReflector(const UsageReflector& parent,
                   const std::string& pathToUse,
                   bool repeatedToUse,
                   const Miro::Options& options);

    void record(std::string_view type, const std::string& defaultValue);
    Line* findRecordedLine();

    Vector<Line>& lines;
    std::string path;
    bool optional = false;
    bool repeated = false;
    OwningPointer<UsageReflector> currentChild;
};
} // namespace Detail

template <typename T>
Report CommandLine::fill(T& value) const
{
    auto report = Report {};
    auto reflector = Detail::CommandLineReflector {*this, report};
    Miro::Detail::reflectValue(reflector, value);
    reflector.finish();
    return report;
}

template <typename T>
T CommandLine::get() const
{
    auto value = T {};
    fill(value);
    return value;
}

template <typename T>
std::string CommandLine::usage(std::string_view program)
{
    auto lines = Vector<Detail::UsageReflector::Line> {};
    auto defaults = T {};

    auto schemaWalk = Detail::UsageReflector {
        lines, Miro::Detail::topLevelOptions<T>(Miro::Mode::Save, true)};
    Miro::Detail::reflectValue(schemaWalk, defaults);

    // Schema mode announces an enum's names but never its value, and it walks
    // an optional or a Vector through a default-constructed inner value, so a
    // second plain Save pass is what fills each leaf's default text in.
    auto defaultsWalk = Detail::UsageReflector {
        lines, Miro::Detail::topLevelOptions<T>(Miro::Mode::Save)};
    Miro::Detail::reflectValue(defaultsWalk, defaults);

    return Detail::UsageReflector::render(program, lines);
}

template <typename T>
T Argument::as(const T& fallback) const
{
    if (!present)
        return fallback;

    auto args = Vector<std::string> {"--value", value};
    if (bare)
        args.resize(1);

    struct Holder
    {
        T value;
        MIRO_REFLECT(value)
    };

    auto holder = Holder {fallback};
    if (!CommandLine {args}.fill(holder).ok())
        return fallback;

    return holder.value;
}
} // namespace eacp::Apps
