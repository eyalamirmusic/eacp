#include "ExportList.h"

#include <eacp/Core/App/Args.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Format.h>

#include <chrono>
#include <algorithm>

using namespace eacp;
using namespace eacp::ExportList;

namespace
{
constexpr auto toolName = std::string_view {"eacp-export-list-tool"};

struct Settings
{
    FilePath output;
    std::string imageName;
    Vector<FilePath> responseFiles;
    Vector<FilePath> inputs;
    FilePath report;
    bool unfiltered = false;
    std::size_t warnAt = ExportLimits {}.warnAt;
    std::size_t limit = ExportLimits {}.maximum;
};

std::string_view unquoted(std::string_view text)
{
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        return text.substr(1, text.size() - 2);

    return text;
}

Vector<FilePath> readResponseFile(const FilePath& path)
{
    if (!Files::isFile(path))
        throw Error(Strings::concat(path.str(), ": the input list does not exist"));

    auto paths = Vector<FilePath> {};
    auto text = Files::readFile(path);
    auto start = std::size_t {0};

    while (start < text.size())
    {
        auto end = std::min(text.find('\n', start), text.size());
        auto trimmed =
            Strings::trim(std::string_view {text}.substr(start, end - start));
        auto line = unquoted(trimmed);

        if (!line.empty())
            paths.add(FilePath {line});

        start = end + 1;
    }

    return paths;
}

Vector<FilePath> allInputs(const Settings& settings)
{
    auto inputs = settings.inputs;

    for (const auto& responseFile: settings.responseFiles)
        inputs.addFrom(readResponseFile(responseFile));

    if (inputs.empty())
        throw Error("no inputs: pass objects and archives, or --inputs <list>");

    return inputs;
}

std::string_view verdictName(Verdict verdict)
{
    switch (verdict)
    {
        case Verdict::Export:
            return "export";
        case Verdict::ExportData:
            return "export-data";
        case Verdict::ExcludedByName:
            return "excluded-name";
        case Verdict::ReadOnlyData:
            return "read-only-data";
        case Verdict::ComdatData:
            return "comdat-data";
        case Verdict::StandardLibrary:
            return "std";
    }

    return "";
}

double percentOf(std::size_t count, std::size_t limit)
{
    return 100.0 * static_cast<double>(count) / static_cast<double>(limit);
}

void checkCount(const Settings& settings, std::size_t count)
{
    auto limits = ExportLimits {settings.warnAt, settings.limit};
    auto status = checkLimit(count, limits);

    if (status == LimitStatus::OverLimit)
        throw Error(Strings::format(
            "{} would export {} symbols, more than the {} a Windows image can "
            "export. Remove libraries the hot-reload module does not use from the "
            "app's closure, or split eacp into a DLL of its own "
            "(Lib/eacp/ExportList/README.md).",
            settings.imageName,
            count,
            settings.limit));

    if (status == LimitStatus::NearLimit)
        Console::printError(Strings::format(
            "{}: warning: {} exports {} symbols, {:.0f}% of the {} a Windows image "
            "can export; the build fails above that",
            toolName,
            settings.imageName,
            count,
            percentOf(count, settings.limit),
            settings.limit));
}

std::string_view kindName(SymbolKind kind)
{
    switch (kind)
    {
        case SymbolKind::Code:
            return "code";
        case SymbolKind::Data:
            return "data";
        case SymbolKind::ReadOnlyData:
            return "read-only";
    }

    return "";
}

// One line a symbol, tab-separated: its verdict, kind, whether it is a COMDAT,
// its decorated name and its undecorated one.
void writeReport(const FilePath& path, const Collector& collector, Filter filter)
{
    auto lines = Vector<std::string> {};

    collector.forEachSymbol(
        [&](std::string_view name, const SymbolFacts& facts)
        {
            lines.add(
                Strings::concat(verdictName(classifySymbol(name, facts, filter)),
                                "\t",
                                kindName(facts.kind),
                                facts.comdat ? "\tcomdat\t" : "\tunique\t",
                                name,
                                "\t",
                                undecorate(name)));
        });

    std::sort(lines.begin(), lines.end());

    auto text = std::string {};

    for (const auto& line: lines)
        text += line + "\n";

    Files::writeFile(path, text);
}

int run(const Settings& settings)
{
    auto started = std::chrono::steady_clock::now();
    auto collector = Collector {};
    auto inputs = allInputs(settings);

    for (const auto& input: inputs)
        collector.addFile(input);

    auto filter = settings.unfiltered ? Filter::Unfiltered : Filter::HotReload;
    auto statistics = Statistics {};
    auto exports = collector.exports(filter, &statistics);

    if (exports.empty())
        throw Error(Strings::concat(
            "the inputs define nothing to export; refusing to write an empty list"));

    checkCount(settings, exports.size());

    auto written = writeIfChanged(settings.output,
                                  makeDefinitionFile(settings.imageName, exports));

    if (!settings.report.str().empty())
        writeReport(settings.report, collector, filter);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    Console::print(Strings::format(
        "{}: {} exports {} symbols ({:.1f}% of {}) from {} objects in {} files, "
        "{} ms; {} std, {} COMDAT data dropped; {}",
        toolName,
        settings.imageName,
        exports.size(),
        percentOf(exports.size(), settings.limit),
        settings.limit,
        statistics.objects,
        statistics.files,
        elapsed.count(),
        statistics.standardLibrary,
        statistics.comdatData,
        written ? "written" : "unchanged"));

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    auto settings = Settings {};

    auto cli = Args::Parser {
        std::string {toolName},
        "Lists the symbols an executable exports to its hot-reload module, as a "
        "module-definition file, from the COFF objects and archives it links."};
    cli.option("--output", "-o", settings.output, "the .def file to write")
        .hint("<file>")
        .required();
    cli.option("--name", settings.imageName, "the image the exports live in")
        .hint("<App.exe>")
        .required();
    cli.option(
           "--inputs", settings.responseFiles, "a file listing inputs, one a line")
        .hint("<file>");
    cli.flag("--unfiltered", settings.unfiltered, "only CMake __create_def's rules");
    cli.option("--warn-at", settings.warnAt, "warn at this many exports").atLeast(1);
    cli.option("--limit", settings.limit, "fail above this many exports").atLeast(1);
    cli.option("--report", settings.report, "write every symbol and its verdict")
        .hint("<file>");
    cli.positional(settings.inputs, "input");
    cli.epilogue("Exit codes: 0 listed, 1 an input or the list was refused, "
                 "2 bad arguments.");

    if (auto code = cli.exitCodeFor(cli.parse(argc, argv)))
        return *code;

    try
    {
        return run(settings);
    }
    catch (const std::exception& error)
    {
        Files::removeFile(settings.output);
        Console::printError(Strings::concat(toolName, ": error: ", error.what()));
        return 1;
    }
}
