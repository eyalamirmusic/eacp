#include "Common.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/App/Args.h>
#include <eacp/Core/Utils/Environment.h>

#include <sstream>

using namespace nano;

namespace ArgsTests
{
using eacp::FilePath;
using eacp::Vector;
using eacp::Args::Parser;
using eacp::Args::Result;

enum class Format
{
    vst3,
    au,
    clap
};

enum class Mode
{
    fast,
    thorough
};

constexpr auto testVariable = "EACP_ARGS_TEST_FOLDER";

bool containsText(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

bool failedWith(const Result& result, std::string_view error)
{
    return !result.ok && !result.help && result.error == error;
}

auto tStringOption = test("Args/a string option takes the next argument") = []
{
    auto output = std::string {};
    auto cli = Parser {"Tool"};
    cli.option("--output", output, "where to write");

    auto result = cli.parse({"--output", "cache.db"});

    check(result.ok);
    check(output == "cache.db");
};

auto tInlineValue = test("Args/--key=value is the same as --key value") = []
{
    auto output = std::string {};
    auto jobs = 0;
    auto cli = Parser {"Tool"};
    cli.option("--output", output, "where to write");
    cli.option("--jobs", jobs, "workers");

    check(cli.parse({"--output=a=b", "--jobs=4"}).ok);
    check(output == "a=b");
    check(jobs == 4);
};

auto tShortAlias = test("Args/a short alias reaches the same option") = []
{
    auto output = std::string {};
    auto cli = Parser {"Tool"};
    cli.option("--output", "-o", output, "where to write");

    check(cli.parse({"-o", "x.db"}).ok);
    check(output == "x.db");
};

auto tNumbers = test("Args/integer, floating and FilePath values") = []
{
    auto jobs = 0;
    auto count = std::uint64_t {};
    auto gain = 0.0;
    auto ratio = 0.0f;
    auto offset = 0;
    auto file = FilePath {};
    auto cli = Parser {"Tool"};
    cli.option("--jobs", jobs, "workers");
    cli.option("--count", count, "how many");
    cli.option("--gain", gain, "gain");
    cli.option("--ratio", ratio, "ratio");
    cli.option("--offset", offset, "negative is fine");
    cli.option("--file", file, "a file");

    auto result = cli.parse({"--jobs",
                             "8",
                             "--count",
                             "18446744073709551615",
                             "--gain",
                             "-1.5",
                             "--ratio",
                             "0.25",
                             "--offset",
                             "-5",
                             "--file",
                             "dir/name.txt"});

    check(result.ok);
    check(jobs == 8);
    check(count == 18446744073709551615ull);
    check(gain == -1.5);
    check(ratio == 0.25f);
    check(offset == -5);
    check(file.str() == "dir/name.txt");
};

auto tNegatedFlag = test("Args/a negated flag sets its bool to false") = []
{
    auto useDefaultFolders = true;
    auto cli = Parser {"Tool"};
    cli.flag(
           "--no-default-folders", useDefaultFolders, "scan only the given folders")
        .negated();

    check(cli.parse({}).ok);
    check(useDefaultFolders);
    check(cli.parse({"--no-default-folders"}).ok);
    check(!useDefaultFolders);
    check(failedWith(cli.parse({"--no-default-folders=yes"}),
                     "--no-default-folders takes no value"));

    auto text = cli.helpText();
    check(containsText(text, "scan only the given folders\n"));
    check(!containsText(text, "[default:"));
};

auto tBoolOption = test("Args/a bool option takes a value") = []
{
    auto verify = true;
    auto cli = Parser {"Tool"};
    cli.option("--verify", verify, "check the output");

    check(cli.parse({"--verify", "off"}).ok);
    check(!verify);
    check(cli.parse({"--verify=YES"}).ok);
    check(verify);
    check(failedWith(cli.parse({"--verify", "maybe"}),
                     "--verify needs true or false, not 'maybe'"));
};

auto tFlag = test("Args/a flag takes no value") = []
{
    auto force = false;
    auto cli = Parser {"Tool"};
    cli.flag("--force", "-f", force, "rescan everything");

    check(cli.parse({}).ok);
    check(!force);
    check(cli.parse({"-f"}).ok);
    check(force);
    check(failedWith(cli.parse({"--force=no"}), "--force takes no value"));
};

auto tEnum = test("Args/an enum is parsed by name, in any case") = []
{
    auto mode = Mode::fast;
    auto cli = Parser {"Tool"};
    cli.option("--mode", mode, "how hard to look");

    check(cli.parse({"--mode", "thorough"}).ok);
    check(mode == Mode::thorough);
    check(cli.parse({"--mode", "FAST"}).ok);
    check(mode == Mode::fast);
};

auto tBadEnum = test("Args/an unknown enum name lists the choices") = []
{
    auto formats = Vector<Format> {};
    auto cli = Parser {"Tool"};
    cli.option("--format", formats, "plugin formats");

    check(failedWith(cli.parse({"--format", "aax"}),
                     "--format must be one of vst3, au, clap, not 'aax'"));
};

auto tRepeatable = test("Args/a Vector option repeats") = []
{
    auto folders = Vector<std::string> {"default"};
    auto formats = Vector<Format> {};
    auto cli = Parser {"Tool"};
    cli.option("--folder", folders, "a folder to scan");
    cli.option("--format", formats, "plugin formats");

    check(cli.parse({"--folder", "a", "--format", "au", "--folder", "b"}).ok);
    check(folders == Vector<std::string> {"a", "b"});
    check(formats == Vector<Format> {Format::au});
};

auto tRepeatSkipsDuplicates =
    test("Args/a Vector option skips a repeated value") = []
{
    auto folders = Vector<std::string> {};
    auto formats = Vector<Format> {};
    auto cli = Parser {"Tool"};
    cli.option("--folder", folders, "a folder to scan").unique();
    cli.option("--format", formats, "plugin formats").unique();

    check(cli.parse({"--folder",
                     "a",
                     "--folder",
                     "b",
                     "--folder",
                     "a",
                     "--format",
                     "au",
                     "--format",
                     "AU"})
              .ok);
    check(folders == Vector<std::string> {"a", "b"});
    check(formats == Vector<Format> {Format::au});
};

auto tRepeatKeepsDuplicates =
    test("Args/a Vector option keeps a repeated value without unique") = []
{
    auto folders = Vector<std::string> {};
    auto cli = Parser {"Tool"};
    cli.option("--folder", folders, "a folder to scan");

    check(cli.parse({"--folder", "a", "--folder", "b", "--folder", "a"}).ok);
    check(folders == Vector<std::string> {"a", "b", "a"});
};

auto tRequired = test("Args/a required option or positional must be given") = []
{
    auto format = std::string {};
    auto input = FilePath {};
    auto output = std::string {};
    auto verbose = false;
    auto cli = Parser {"eacp-icon-tool"};
    cli.option("--format", format, "the icon format").required();
    cli.option("--in", input, "the source image").hint("<file>").required();
    cli.flag("--verbose", verbose, "say more");
    cli.positional(output, "output", "where to write").required();

    check(failedWith(cli.parse({"--in", "a.png", "out"}),
                     "--format is required (try --help)"));
    check(failedWith(cli.parse({"--format", "icns", "--in", "a.png"}),
                     "output is required (try --help)"));
    check(cli.parse({"--format", "icns", "--in", "a.png", "out"}).ok);

    auto text = cli.helpText();
    check(text.starts_with(
        "Usage: eacp-icon-tool --format <value> --in <file> [options] output\n"));
    check(containsText(text, "the icon format (required)"));
    check(containsText(text, "where to write (required)"));
};

auto tRequiredFromEnv = test("Args/an environment value satisfies required") = []
{
    auto folder = std::string {};
    auto cli = Parser {"Tool"};
    cli.option("--folder", folder, "a folder").required().fromEnv(testVariable);

    eacp::setEnv(testVariable, "from-env");
    check(cli.parse({}).ok);
    check(folder == "from-env");

    eacp::unsetEnv(testVariable);
    check(failedWith(cli.parse({}), "--folder is required (try --help)"));
};

auto tNarrowRanges = test("Args/bounds on a narrow integer are checked first") = []
{
    auto port = std::uint16_t {8080};
    auto bare = std::uint16_t {};
    auto cli = Parser {"Tool"};
    cli.option("--port", port, "the port").atLeast(1).atMost(1024);
    cli.option("--bare", bare, "no bounds");

    check(failedWith(cli.parse({"--port", "70000"}),
                     "--port must be at most 1024, not '70000'"));
    check(failedWith(cli.parse({"--port", "2000"}),
                     "--port must be at most 1024, not '2000'"));
    check(failedWith(cli.parse({"--port", "0"}),
                     "--port must be at least 1, not '0'"));
    check(failedWith(cli.parse({"--port", "-5"}),
                     "--port must be at least 1, not '-5'"));
    check(failedWith(cli.parse({"--bare", "70000"}),
                     "--bare must be from 0 to 65535, not '70000'"));
    check(cli.parse({"--port", "1024"}).ok);
    check(port == 1024);
};

auto tFullWidthRange = test("Args/atMost at the type's maximum still applies") = []
{
    auto port = std::uint16_t {};
    auto cli = Parser {"Tool"};
    cli.option("--port", port, "the port").atLeast(1).atMost(65535);

    check(failedWith(cli.parse({"--port", "70000"}),
                     "--port must be at most 65535, not '70000'"));
    check(cli.parse({"--port", "65535"}).ok);
    check(port == 65535);
};

auto tOptionalTargets = test("Args/an optional target is set only when given") = []
{
    auto port = std::optional<std::uint16_t> {};
    auto name = std::optional<std::string> {};
    auto cli = Parser {"Tool"};
    cli.option("--port", port, "the port").atLeast(1);
    cli.option("--name", name, "a name").fromEnv(testVariable);

    eacp::unsetEnv(testVariable);
    check(cli.parse({}).ok);
    check(!port.has_value());
    check(!name.has_value());

    check(cli.parse({"--port", "9000", "--name", ""}).ok);
    check(port == std::uint16_t {9000});
    check(name == std::string {});

    check(failedWith(cli.parse({"--port", "0"}),
                     "--port must be at least 1, not '0'"));

    eacp::setEnv(testVariable, "env-name");
    auto envName = std::optional<std::string> {};
    auto envCli = Parser {"Tool"};
    envCli.option("--name", envName, "a name").fromEnv(testVariable);
    check(envCli.parse({}).ok);
    check(envName == std::string {"env-name"});
    eacp::unsetEnv(testVariable);

    auto text = cli.helpText();
    check(containsText(text, "--port <n>"));
    check(containsText(text, "the port\n"));
    check(!containsText(text, "[default:"));
};

auto tOptionalPositional =
    test("Args/an optional positional is set only when given") = []
{
    auto count = std::optional<int> {};
    auto cli = Parser {"Tool"};
    cli.positional(count, "count", "how many").atMost(10).defaultText("all");

    check(cli.parse({}).ok);
    check(!count.has_value());
    check(cli.parse({"3"}).ok);
    check(count == 3);
    check(failedWith(cli.parse({"11"}), "count must be at most 10, not '11'"));
    check(containsText(cli.helpText(), "how many [default: all]"));
};

auto tPositionalBuilders =
    test("Args/positionals take the builder's refinements") = []
{
    auto files = Vector<std::string> {"a.txt"};
    auto mode = std::string {"fast"};
    auto cli = Parser {"Tool"};
    cli.positional(mode, "mode", "how hard to look").choices({"fast", "thorough"});
    cli.positional(files, "file", "files to read").unique();

    check(cli.parse({"THOROUGH", "x", "y", "x"}).ok);
    check(mode == "thorough");
    check(files == Vector<std::string> {"x", "y"});
    check(failedWith(cli.parse({"slow"}),
                     "mode must be one of fast, thorough, not 'slow'"));

    auto text = cli.helpText();
    check(containsText(text,
                       "how hard to look (one of fast, thorough) [default: fast]"));
    check(containsText(text, "files to read (repeatable) [default: a.txt]"));
};

auto tFail = test("Args/fail returns badArguments for a post-parse problem") = []
{
    auto cli = Parser {"Tool"};
    check(cli.fail("nothing to validate") == eacp::Args::badArguments);
};

auto tEnumChoices = test("Args/choices restricts an enum to a subset") = []
{
    auto formats = Vector<Format> {};
    auto cli = Parser {"Tool"};
    cli.option("--format", formats, "plugin formats")
        .choices({Format::vst3, Format::au});

    check(cli.parse({"--format", "VST3", "--format", "au"}).ok);
    check(formats == Vector<Format> {Format::vst3, Format::au});
    check(failedWith(cli.parse({"--format", "clap"}),
                     "--format must be one of vst3, au, not 'clap'"));
    check(failedWith(cli.parse({"--format", "aax"}),
                     "--format must be one of vst3, au, not 'aax'"));

    auto text = cli.helpText();
    check(containsText(text, "(repeatable; one of vst3, au)"));
    check(!containsText(text, "clap"));
};

auto tStringChoices = test("Args/choices restricts a string, in any case") = []
{
    auto level = std::string {"info"};
    auto cli = Parser {"Tool"};
    cli.option("--log", level, "how much to say")
        .choices({"quiet", "info", "debug"});

    check(cli.parse({"--log", "DEBUG"}).ok);
    check(level == "debug");
    check(failedWith(cli.parse({"--log", "loud"}),
                     "--log must be one of quiet, info, debug, not 'loud'"));
    check(
        containsText(cli.helpText(),
                     "how much to say (one of quiet, info, debug) [default: info]"));
};

auto tHintAndDefaultText =
    test("Args/hint and defaultText replace the generated ones") = []
{
    auto jobs = 0;
    auto timeout = 0;
    auto cli = Parser {"Tool"};
    cli.option("--jobs", jobs, "parallel workers")
        .hint("<count>")
        .defaultText("the core count");
    cli.option("--timeout", timeout, "per-plugin timeout").hint("<ms>");

    auto text = cli.helpText();
    check(containsText(text, "--jobs <count>"));
    check(containsText(text, "parallel workers [default: the core count]"));
    check(containsText(text, "--timeout <ms>"));
    check(containsText(text, "per-plugin timeout\n"));
};

auto tPositionals = test("Args/positionals collect the non-option arguments") = []
{
    auto bundles = Vector<FilePath> {};
    auto force = false;
    auto cli = Parser {"Tool"};
    cli.flag("--force", force, "rescan");
    cli.positional(bundles, "bundle");

    check(cli.parse({"a.vst3", "--force", "b.vst3"}).ok);
    check(force);
    check(bundles.size() == 2);
    check(bundles[0].str() == "a.vst3");
    check(bundles[1].str() == "b.vst3");
};

auto tSinglePositionals = test("Args/single positionals fill in order") = []
{
    auto source = std::string {};
    auto count = 0;
    auto cli = Parser {"Tool"};
    cli.positional(source, "source");
    cli.positional(count, "count");

    check(cli.parse({"in.txt", "3"}).ok);
    check(source == "in.txt");
    check(count == 3);
    check(failedWith(cli.parse({"in.txt", "x"}),
                     "count needs a whole number, not 'x'"));
    check(failedWith(cli.parse({"a", "1", "extra"}),
                     "unknown argument 'extra' (try --help)"));
};

auto tEndOfOptions = test("Args/-- makes the rest positional") = []
{
    auto files = Vector<std::string> {};
    auto force = false;
    auto cli = Parser {"Tool"};
    cli.flag("--force", force, "rescan");
    cli.positional(files, "file");

    check(cli.parse({"--", "--force", "-h", "-"}).ok);
    check(!force);
    check(files == Vector<std::string> {"--force", "-h", "-"});
};

auto tUnknownOption = test("Args/an unknown option is an error") = []
{
    auto cli = Parser {"Tool"};

    check(
        failedWith(cli.parse({"--nope"}), "unknown argument '--nope' (try --help)"));
    check(failedWith(cli.parse({"--nope=1"}),
                     "unknown argument '--nope' (try --help)"));
};

auto tStrayArgument = test("Args/an argument with no positional is an error") = []
{
    auto cli = Parser {"Tool"};

    check(failedWith(cli.parse({"stray"}), "unknown argument 'stray' (try --help)"));
};

auto tMissingValue =
    test("Args/an option at the end or before another needs a value") = []
{
    auto output = std::string {};
    auto force = false;
    auto cli = Parser {"Tool"};
    cli.option("--output", output, "where to write");
    cli.flag("--force", force, "rescan");

    check(failedWith(cli.parse({"--output"}), "--output needs a value"));
    check(failedWith(cli.parse({"--output", "--force"}), "--output needs a value"));
    check(!force);
};

auto tBadInteger = test("Args/a bad integer names the option and the text") = []
{
    auto jobs = 0;
    auto small = std::int8_t {};
    auto cli = Parser {"Tool"};
    cli.option("--jobs", jobs, "workers");
    cli.option("--small", small, "tiny");

    check(failedWith(cli.parse({"--jobs", "abc"}),
                     "--jobs needs a whole number, not 'abc'"));
    check(failedWith(cli.parse({"--jobs", "1.5"}),
                     "--jobs needs a whole number, not '1.5'"));
    check(failedWith(cli.parse({"--small", "300"}),
                     "--small must be from -128 to 127, not '300'"));
};

auto tBadFloating = test("Args/a bad number names the option and the text") = []
{
    auto gain = 0.0;
    auto cli = Parser {"Tool"};
    cli.option("--gain", gain, "gain");

    check(failedWith(cli.parse({"--gain", "loud"}),
                     "--gain needs a number, not 'loud'"));
};

auto tRanges = test("Args/atLeast and atMost bound a value") = []
{
    auto jobs = 1;
    auto timeouts = Vector<int> {};
    auto cli = Parser {"Tool"};
    cli.option("--jobs", jobs, "workers").atLeast(1).atMost(64);
    cli.option("--timeout", timeouts, "per-plugin timeouts").atLeast(0);

    check(failedWith(cli.parse({"--jobs", "0"}),
                     "--jobs must be at least 1, not '0'"));
    check(failedWith(cli.parse({"--jobs", "65"}),
                     "--jobs must be at most 64, not '65'"));
    check(failedWith(cli.parse({"--timeout", "5", "--timeout", "-1"}),
                     "--timeout must be at least 0, not '-1'"));
    check(cli.parse({"--jobs", "64"}).ok);
    check(jobs == 64);
};

auto tCustomCheck = test("Args/check runs a rule of the caller's") = []
{
    auto name = std::string {};
    auto cli = Parser {"Tool"};
    cli.option("--name", name, "a name")
        .check(
            [](const std::string& value) -> std::optional<std::string>
            {
                if (value.find('/') != std::string::npos)
                    return "must not contain '/'";

                return std::nullopt;
            });

    check(failedWith(cli.parse({"--name", "a/b"}),
                     "--name must not contain '/', not 'a/b'"));
    check(cli.parse({"--name", "ab"}).ok);
    check(name == "ab");
};

auto tStopsAtFirstError = test("Args/parsing stops at the first error") = []
{
    auto first = 0;
    auto second = 0;
    auto cli = Parser {"Tool"};
    cli.option("--first", first, "first");
    cli.option("--second", second, "second");

    check(failedWith(cli.parse({"--first", "x", "--second", "2"}),
                     "--first needs a whole number, not 'x'"));
    check(second == 0);
};

auto tNullArgv =
    test("Args/parse(argc, argv) skips the program and reads null as empty") = []
{
    auto files = Vector<std::string> {};
    auto cli = Parser {"Tool"};
    cli.positional(files, "file");

    const char* argv[] = {"tool", "a", nullptr, "b"};

    check(cli.parse(4, argv).ok);
    check(files == Vector<std::string> {"a", "", "b"});
};

auto tFromEnv = test("Args/fromEnv sets a default the command line overrides") = []
{
    auto folder = std::string {"built-in"};
    auto cli = Parser {"Tool"};
    cli.option("--folder", folder, "a folder").fromEnv(testVariable);

    eacp::unsetEnv(testVariable);
    check(cli.parse({}).ok);
    check(folder == "built-in");

    eacp::setEnv(testVariable, "");
    check(cli.parse({}).ok);
    check(folder == "built-in");

    eacp::setEnv(testVariable, "from-env");
    check(cli.parse({}).ok);
    check(folder == "from-env");

    check(cli.parse({"--folder", "from-argv"}).ok);
    check(folder == "from-argv");

    eacp::unsetEnv(testVariable);
};

auto tFromEnvChecked = test("Args/fromEnv values are parsed and checked") = []
{
    auto jobs = 1;
    auto folders = Vector<std::string> {};
    auto cli = Parser {"Tool"};
    cli.option("--jobs", jobs, "workers").atLeast(1).fromEnv(testVariable);

    eacp::setEnv(testVariable, "0");
    check(failedWith(
        cli.parse({}),
        "EACP_ARGS_TEST_FOLDER (for --jobs) must be at least 1, not '0'"));

    eacp::setEnv(testVariable, "3");
    check(cli.parse({}).ok);
    check(jobs == 3);

    auto vectorCli = Parser {"Tool"};
    vectorCli.option("--folder", folders, "folders").fromEnv(testVariable);

    check(vectorCli.parse({"--folder", "a", "--folder", "b"}).ok);
    check(folders == Vector<std::string> {"a", "b"});

    eacp::unsetEnv(testVariable);
};

auto tHelp = test("Args/-h and --help ask for help") = []
{
    auto jobs = 0;
    auto cli = Parser {"Tool"};
    cli.option("--jobs", jobs, "workers");

    for (auto arg: {"-h", "--help"})
    {
        auto result = cli.parse({"--jobs", "x", arg});
        check(result.help);
        check(!result.ok);
    }
};

auto tHelpText = test("Args/help text lists every option and its default") = []
{
    auto output = std::string {"cache.db"};
    auto folders = Vector<std::string> {};
    auto formats = Vector<Format> {Format::vst3, Format::au};
    auto force = false;
    auto jobs = 4;
    auto timeout = 0;
    auto bundles = Vector<std::string> {};

    auto cli = Parser {"PluginScanner", "Describes every plugin into a database."};
    cli.option("--output", "-o", output, "the database to write");
    cli.option("--folder", folders, "a folder to scan").fromEnv("SCAN_FOLDER");
    cli.option("--format", formats, "plugin formats");
    cli.flag("--force", force, "rescan plugins already in the database");
    cli.option("--jobs", jobs, "parallel workers").atLeast(1);
    cli.option("--timeout", timeout, "per-plugin timeout in ms");
    cli.positional(bundles, "bundle");
    cli.epilogue("Exit codes:\n  0  success");

    // Parsing changes the variables; the help still shows the code's defaults.
    check(cli.parse({"--jobs", "9", "--output", "other.db"}).ok);

    auto text = cli.helpText();

    check(text.starts_with("Usage: PluginScanner [options] [bundle...]"));
    check(containsText(text, "Describes every plugin into a database."));
    check(containsText(text, "-o, --output <value>"));
    check(containsText(text, "the database to write [default: cache.db]"));
    check(containsText(text, "--folder <value>"));
    check(containsText(text, "a folder to scan (repeatable) [env: SCAN_FOLDER]"));
    check(containsText(text, "--format <name>"));
    // Too long for the 56 columns left beside the names, so it wraps under
    // the description column.
    check(containsText(text,
                       "plugin formats (repeatable; one of vst3, au, clap)\n"
                           + std::string(24, ' ') + "[default: vst3, au]"));
    check(containsText(text, "--force "));
    check(!containsText(text, "--force <"));
    check(containsText(text, "parallel workers [default: 4]"));
    check(!containsText(text, "[default: 9]"));
    check(containsText(text, "per-plugin timeout in ms\n"));
    check(containsText(text, "-h, --help"));
    check(text.ends_with("Exit codes:\n  0  success"));
};

Vector<std::string> linesOf(const std::string& text)
{
    auto lines = Vector<std::string> {};
    auto stream = std::istringstream {text};

    for (auto line = std::string {}; std::getline(stream, line);)
        lines.add(line);

    return lines;
}

auto tHelpWraps = test("Args/help text wraps at 80 columns") = []
{
    constexpr auto longWord = "https://example.com/a/really/long/path/that/cannot/"
                              "be/broken/anywhere/because/it/has/no/spaces";
    auto jobs = 0;
    auto url = std::string {};
    auto cli = Parser {"Tool",
                       "A top description that is long enough to need wrapping "
                       "because it runs well past the eighty columns a terminal "
                       "shows."};
    cli.option("--jobs",
               jobs,
               "the number of parallel workers to run, each one a separate "
               "process that describes one plugin at a time and reports back "
               "over a pipe");
    cli.option("--url", url, std::string {"see "} + longWord);
    cli.epilogue("Exit codes:\n  0  success\n  5  a plugin crashed while it was "
                 "being described, and the database records it so the next run "
                 "skips it\n\nReport bugs upstream.");

    auto text = cli.helpText();

    // "    --url <value>" is the longest name, so descriptions start at column
    // 2 + 17 + 2.
    auto column = std::string(21, ' ');

    for (const auto& line: linesOf(text))
        check(line.size() <= 80 || line == column + longWord);

    check(containsText(text,
                       "A top description that is long enough to need wrapping "
                       "because it runs well past\nthe eighty columns a terminal "
                       "shows.\n"));
    check(containsText(text,
                       "      --jobs <n>     the number of parallel workers to "
                       "run, each one a separate\n"
                           + column
                           + "process that describes one plugin at a time and "
                             "reports\n"
                           + column + "back over a pipe\n"));
    check(
        containsText(text, "      --url <value>  see\n" + column + longWord + "\n"));
    check(text.ends_with("Exit codes:\n"
                         "  0  success\n"
                         "  5  a plugin crashed while it was being described, and "
                         "the database records it\n"
                         "  so the next run skips it\n"
                         "\n"
                         "Report bugs upstream."));
};

auto tAllBuilders = test("Args/choices, hint and defaultText together") = []
{
    auto formats = Vector<Format> {};
    auto cli = Parser {"PluginScanner"};
    cli.option("--format", formats, "plugin formats to scan")
        .choices({Format::vst3, Format::au})
        .hint("<format>")
        .defaultText("every format the platform has");

    check(containsText(cli.helpText(),
                       "      --format <format>  plugin formats to scan "
                       "(repeatable; one of vst3, au)\n"
                       "                         [default: every format the "
                       "platform has]\n"));
};

auto tHelpColumns = test("Args/help descriptions line up in one column") = []
{
    auto a = 0;
    auto longerName = 0;
    auto cli = Parser {"Tool"};
    cli.option("--a", a, "first");
    cli.option("--a-much-longer-name", longerName, "second");

    auto text = cli.helpText();
    auto column = [&text](std::string_view description)
    {
        auto at = text.find(description);
        return at - text.rfind('\n', at) - 1;
    };

    check(column("first") == column("second"));
    check(column("second") == column("show this help"));
};

auto tExitCodes = test("Args/exitCodeFor says whether to stop and with what") = []
{
    auto jobs = 0;
    auto cli = Parser {"Tool"};
    cli.option("--jobs", jobs, "workers");

    check(!cli.exitCodeFor(cli.parse({"--jobs", "2"})).has_value());
    check(cli.exitCodeFor(cli.parse({"--jobs", "x"})) == eacp::Args::badArguments);
    check(eacp::Args::badArguments == 2);
};

auto tStoredCommandLine =
    test("Args/parse() reads the stored command line without the program") = []
{
    namespace Apps = eacp::Apps;

    const auto saved = Apps::commandLineArgs();

    char program[] = "/bin/Tool";
    char name[] = "--jobs";
    char value[] = "3";
    char bundle[] = "Plugin.vst3";
    char* argv[] = {program, name, value, bundle};
    Apps::setCommandLineArgs(4, argv);

    auto jobs = 0;
    auto bundles = Vector<std::string> {};
    auto cli = Parser {"Tool"};
    cli.option("--jobs", jobs, "workers");
    cli.positional(bundles, "bundle");

    auto result = cli.parse();
    Apps::getAppEnvironment().commandLineArgs = saved;

    check(result.ok);
    check(jobs == 3);
    check(bundles.size() == 1);
    check(bundles[0] == "Plugin.vst3");
};
} // namespace ArgsTests
