#include "Common.h"

#include <eacp/Core/App/CommandLine.h>

using namespace nano;
using eacp::Apps::CommandLine;

namespace
{
CommandLine parse(const eacp::Vector<std::string>& args)
{
    return CommandLine {args};
}

void addSwitch(eacp::Vector<std::string>& args,
               const std::string& name,
               const std::string& value)
{
    args.add("--" + name);
    args.add(value);
}

bool mentions(const eacp::Vector<std::string>& lines, std::string_view needle)
{
    for (const auto& line: lines)
        if (line.find(needle) != std::string::npos)
            return true;

    return false;
}

std::string lineWith(const std::string& text, std::string_view needle)
{
    const auto at = text.find(needle);

    if (at == std::string::npos)
        return {};

    const auto previous = text.rfind('\n', at);
    const auto start =
        previous == std::string::npos ? std::size_t {0} : previous + 1;
    const auto end = text.find('\n', at);

    return text.substr(start, end == std::string::npos ? end : end - start);
}

enum class Mode
{
    fast,
    slow
};

struct Server
{
    bool operator==(const Server& other) const = default;

    int port = 80;

    MIRO_REFLECT(port)
};

struct Options
{
    bool operator==(const Options& other) const = default;

    std::string input;
    int port = 8080;
    bool verbose = false;
    std::optional<std::string> logFile;
    eacp::Vector<std::string> include;
    Mode mode = Mode::fast;
    Server server;
    double ratio = 0.5;
    std::int64_t big = 0;

    MIRO_REFLECT(input, port, verbose, logFile, include, mode, server, ratio, big)
};
} // namespace

auto tEmpty = test("CommandLine/no arguments is empty") = []
{
    const auto cl = parse({});

    check(cl.empty());
    check(cl.command.empty());
    check(cl.positionals.empty());
    check(cl.options.empty());
    check(!parse({"build"}).empty());
};

auto tValueForms = test("CommandLine/--name value and --name=value agree") = []
{
    const auto spaced = parse({"--name", "value"})["name"];
    const auto equals = parse({"--name=value"})["name"];

    for (const auto& argument: {spaced, equals})
    {
        check(argument.present);
        check(!argument.bare);
        check(argument.value == "value");
    }
};

auto tBareSwitch = test("CommandLine/a switch with nothing to take is bare") = []
{
    const auto atEnd = parse({"--verbose"})["verbose"];

    check(atEnd.present);
    check(atEnd.bare);
    check(atEnd.value.empty());

    const auto beforeSwitch = parse({"--verbose", "--quiet"})["verbose"];

    check(beforeSwitch.present);
    check(beforeSwitch.bare);
};

auto tEmptyValue = test("CommandLine/--name= is present with an empty value") = []
{
    const auto argument = parse({"--name="})["name"];

    check(argument.present);
    check(!argument.bare);
    check(argument.value.empty());
};

auto tNegativeValue =
    test("CommandLine/a value that looks like a number is a value") = []
{
    const auto cl = parse({"--offset", "-5"});

    check(!cl["offset"].bare);
    check(cl["offset"].value == "-5");
    check(cl["offset"].toInt() == -5);
};

auto tCommandAndPositionals =
    test("CommandLine/the first bare word is the command") = []
{
    const auto cl = parse({"build", "--port", "8080", "x", "y"});

    check(cl.command == "build");
    check(cl["port"].toInt() == 8080);
    check(cl.positionals == eacp::Vector<std::string> {"x", "y"});
};

auto tDoubleDash = test("CommandLine/a lone -- ends switch parsing") = []
{
    const auto cl = parse({"build", "--", "--port", "x"});

    check(cl.command == "build");
    check(cl.options.empty());
    check(cl.positionals == eacp::Vector<std::string> {"--port", "x"});
};

auto tSingleDash = test("CommandLine/a single-dash token is not a switch") = []
{
    const auto cl = parse({"run", "-v"});

    check(cl.command == "run");
    check(cl.options.empty());
    check(cl.positionals == eacp::Vector<std::string> {"-v"});
};

auto tRepetitionsKept =
    test("CommandLine/options keep every occurrence in order") = []
{
    const auto cl = parse({"--include", "a", "--include", "b"});

    check(cl.options.size() == 2);
    check(cl.options[0].name == "include");
    check(cl.options[0].argument.value == "a");
    check(cl.options[1].argument.value == "b");

    const auto all = cl.all("include");

    check(all.size() == 2);
    check(all[0].toString() == "a");
    check(all[1].toString() == "b");

    check(cl["include"].toString() == "b");
    check(cl.has("include"));
    check(!cl.has("exclude"));
};

auto tNameAsWritten = test("CommandLine/Option::name is the spelling from argv") = []
{ check(parse({"--output-dir", "out"}).options[0].name == "output-dir"); };

auto tFromArgv = test("CommandLine/fromArgv drops the executable path") = []
{
    char program[] = "tool";
    char command[] = "build";
    char name[] = "--port";
    char value[] = "8080";
    char* argv[] = {program, command, name, value};

    const auto cl = CommandLine::fromArgv(4, argv);

    check(cl.command == "build");
    check(cl["port"].toInt() == 8080);
};

auto tNameMatching = test("CommandLine/names match ignoring case, - and _") = []
{
    const auto cl = parse({"--output-dir", "out"});

    check(cl["output-dir"].toString() == "out");
    check(cl["outputDir"].toString() == "out");
    check(cl["OUTPUT_DIR"].toString() == "out");
    check(cl.has("outputdir"));
    check(cl.all("outputDir").size() == 1);
};

auto tNormalizeOptionName = test("CommandLine/normalizeOptionName keeps only .") = []
{
    using eacp::Apps::normalizeOptionName;

    check(normalizeOptionName("Output-Dir") == "outputdir");
    check(normalizeOptionName("output_dir") == "outputdir");
    check(normalizeOptionName("outputDir") == "outputdir");
    check(normalizeOptionName("server.maxConnections") == "server.maxconnections");
};

auto tKebabCase = test("CommandLine/kebabCase splits on the capitals") = []
{
    using eacp::Apps::kebabCase;

    check(kebabCase("port") == "port");
    check(kebabCase("outputDir") == "output-dir");
    check(kebabCase("server.maxConnections") == "server.max-connections");
};

auto tToBool = test("CommandLine/toBool says no only to the words for no") = []
{
    check(!parse({})["v"].toBool(false));
    check(parse({})["v"].toBool(true));
    check(parse({"--v"})["v"].toBool(false));

    for (const auto* no: {"false", "no", "off", "0", "FALSE", "No", "OFF"})
        check(!parse({"--v", no})["v"].toBool(true));

    for (const auto* yes: {"true", "yes", "on", "1", "TRUE", "anything"})
        check(parse({"--v", yes})["v"].toBool(false));
};

auto tNumbersRejectJunk = test("CommandLine/a number with trailing junk is the "
                               "fallback") = []
{
    check(parse({"--n", "12"})["n"].toInt(42) == 12);
    check(parse({"--n", "12abc"})["n"].toInt(42) == 42);
    check(parse({"--n", "5000000000"})["n"].toInt64(1) == 5000000000LL);
    check(parse({"--n", "5000000000x"})["n"].toInt64(1) == 1);
    check(parse({"--n", "0.25"})["n"].toDouble(1.0) == 0.25);
    check(parse({"--n", "0.25x"})["n"].toDouble(1.0) == 1.0);
    check(parse({"--n", "0.5"})["n"].toFloat(1.f) == 0.5f);
    check(parse({"--n", "0.5x"})["n"].toFloat(1.f) == 1.f);
};

auto tAbsentIsTheFallback = test("CommandLine/an absent switch is the fallback") = []
{
    const auto cl = parse({"--port", "9000"});

    check(cl["missing"].toString("default") == "default");
    check(cl["missing"].empty());
    check(!cl["missing"]);
    check(static_cast<bool>(cl["port"]));
    check(cl["port"].toString("default") == "9000");
};

auto tAs = test("CommandLine/as reads the type the caller asks for") = []
{
    check(parse({"--mode", "slow"})["mode"].as<Mode>(Mode::fast) == Mode::slow);
    check(parse({"--n", "12"})["n"].as<int>(1) == 12);
    check(parse({"--s", "hi"})["s"].as<std::string>("d") == "hi");
    check(parse({})["missing"].as<int>(5) == 5);

    // An unknown enumerator leaves Miro's load untouched, so either way the
    // caller keeps the fallback.
    check(parse({"--mode", "nonsense"})["mode"].as<Mode>(Mode::fast) == Mode::fast);
};

// A bare flag falling through to the parser would read "" and hand back the
// fallback, making `--headless` mean the opposite on a false default.
auto tBareFlagIsItsOwnYes =
    test("CommandLine/a bare switch means yes even when the default is no") = []
{ check(parse({"shell", "--headless"}).getBool("headless", false)); };

auto tGetBoolValueWins =
    test("CommandLine/a switch given a value is that value") = []
{
    check(!parse({"shell", "--headless", "false"}).getBool("headless", true));
    check(parse({"shell", "--headless", "true"}).getBool("headless", false));
    check(!parse({"shell", "--headless=off"}).getBool("headless", true));
    check(parse({"shell", "--headless=1"}).getBool("headless", false));
};

auto tGetBoolAbsent =
    test("CommandLine/getBool on a switch nobody mentioned is the default") = []
{
    check(parse({"shell", "--check"}).getBool("headless", true));
    check(!parse({"shell", "--check"}).getBool("headless", false));
};

auto tFillKeepsDefaults = test("CommandLine/fill leaves absent fields alone") = []
{
    auto options = Options {};
    const auto report = parse({}).fill(options);

    check(report.ok());
    check(report.toString().empty());
    check(options == Options {});
    check(!options.logFile.has_value());
};

auto tFillEveryFieldType = test("CommandLine/fill reads each field's own type") = []
{
    auto args = eacp::Vector<std::string> {"--verbose"};
    addSwitch(args, "input", "in.txt");
    addSwitch(args, "port", "9000");
    addSwitch(args, "log-file", "out.log");
    addSwitch(args, "include", "a");
    addSwitch(args, "include", "b");
    addSwitch(args, "mode", "slow");
    addSwitch(args, "server.port", "9000");
    addSwitch(args, "ratio", "0.25");
    addSwitch(args, "big", "5000000000");

    auto options = Options {};
    const auto report = parse(args).fill(options);

    check(report.ok());
    check(options.input == "in.txt");
    check(options.port == 9000);
    check(options.verbose);
    check(options.logFile.has_value());
    check(options.logFile.value_or(std::string {}) == "out.log");
    check(options.include == eacp::Vector<std::string> {"a", "b"});
    check(options.mode == Mode::slow);
    check(options.server.port == 9000);
    check(options.ratio == 0.25);
    check(options.big == 5000000000LL);
};

auto tFillBoolValue =
    test("CommandLine/a bool field takes the value it is given") = []
{
    auto options = Options {};
    check(parse({"--verbose", "false"}).fill(options).ok());
    check(!options.verbose);

    auto bare = Options {};
    check(parse({"--verbose"}).fill(bare).ok());
    check(bare.verbose);
};

auto tFillSpellingVariants = test("CommandLine/fill accepts every spelling") = []
{
    auto options = Options {};
    const auto report = parse({"--PORT", "9000", "--log_file", "x"}).fill(options);

    check(report.ok());
    check(options.port == 9000);
    check(options.logFile.value_or(std::string {}) == "x");
};

auto tFillBadNumber =
    test("CommandLine/a bad value keeps the default and reports") = []
{
    auto options = Options {};
    const auto report = parse({"--port", "abc"}).fill(options);

    check(!report.ok());
    check(options.port == 8080);
    check(report.errors.size() == 1);
    check(mentions(report.errors, "--port"));
    check(mentions(report.errors, "abc"));
    check(!report.toString().empty());
};

auto tFillBareValueField = test("CommandLine/a bare switch on a value field is an "
                                "error") = []
{
    auto number = Options {};
    check(!parse({"--port"}).fill(number).ok());
    check(number.port == 8080);

    auto text = Options {};
    check(!parse({"--input"}).fill(text).ok());
    check(text.input.empty());
};

auto tFillUnknownSwitch = test("CommandLine/an unknown switch is reported, not "
                               "fatal") = []
{
    auto options = Options {};
    const auto report = parse({"--bogus", "1", "--port", "9000"}).fill(options);

    check(!report.ok());
    check(report.errors.empty());
    check(mentions(report.unknown, "bogus"));
    check(options.port == 9000);
    check(!report.toString().empty());
};

auto tGet = test("CommandLine/get is a default-constructed T, filled") = []
{
    const auto cl = parse({"--port", "9000", "--mode", "slow", "--include", "a"});

    auto filled = Options {};
    cl.fill(filled);

    check(cl.get<Options>() == filled);
};

auto tUsage = test("CommandLine/usage describes T on its own") = []
{
    const auto text = CommandLine::usage<Options>("tool");

    check(text.starts_with("Usage: tool [command] [options]\n"));
    check(text.find("--input <string>") != std::string::npos);
    check(text.find("--include <string>...") != std::string::npos);

    const auto port = lineWith(text, "--port <int>");
    check(!port.empty());
    check(port.find("default 8080") != std::string::npos);

    const auto verbose = lineWith(text, "--verbose");
    check(!verbose.empty());
    check(verbose.find('<') == std::string::npos);

    const auto logFile = lineWith(text, "--log-file <string>");
    check(logFile.find("optional") != std::string::npos);

    const auto mode = lineWith(text, "--mode <fast|slow>");
    check(mode.find("default fast") != std::string::npos);

    const auto nested = lineWith(text, "--server.port <int>");
    check(nested.find("default 80") != std::string::npos);

    const auto ratio = lineWith(text, "--ratio <double>");
    check(ratio.ends_with("default 0.5"));

    const auto big = lineWith(text, "--big <int64>");
    check(big.find("default") == std::string::npos);
};

auto tUsageWithoutProgram = test("CommandLine/usage without a program name") = []
{ check(CommandLine::usage<Options>().starts_with("Usage: [command] [options]")); };
