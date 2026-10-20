#include "HotReloadLoop.h"

namespace eacp::Plugins
{
namespace
{
std::string withoutAnsiEscapes(std::string_view text)
{
    auto plain = std::string {};
    plain.reserve(text.size());

    for (auto index = std::size_t {0}; index < text.size(); ++index)
    {
        auto startsEscape = text[index] == '\x1b' && index + 1 < text.size()
                            && text[index + 1] == '[';

        if (!startsEscape)
        {
            plain += text[index];
            continue;
        }

        index += 2;

        while (index < text.size() && (text[index] < 0x40 || text[index] > 0x7e))
            ++index;
    }

    return plain;
}

std::string_view trimmedLine(std::string_view line)
{
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };

    while (!line.empty() && isSpace(line.front()))
        line.remove_prefix(1);

    while (!line.empty() && isSpace(line.back()))
        line.remove_suffix(1);

    return line;
}

Vector<std::string_view> nonEmptyLines(std::string_view text)
{
    auto lines = Vector<std::string_view> {};

    while (!text.empty())
    {
        auto end = text.find('\n');
        auto line = trimmedLine(text.substr(0, end));

        if (!line.empty())
            lines.add(line);

        if (end == std::string_view::npos)
            break;

        text.remove_prefix(end + 1);
    }

    return lines;
}

bool isNumber(std::string_view text)
{
    if (text.empty())
        return false;

    for (auto c: text)
        if (c < '0' || c > '9')
            return false;

    return true;
}

bool endsWithLineAndColumn(std::string_view location)
{
    auto lastColon = location.rfind(':');

    if (lastColon == std::string_view::npos)
        return false;

    auto column = location.substr(lastColon + 1);
    auto rest = location.substr(0, lastColon);
    auto previousColon = rest.rfind(':');

    if (previousColon == std::string_view::npos)
        return false;

    return isNumber(column) && isNumber(rest.substr(previousColon + 1));
}

bool endsWithParenthesisedLine(std::string_view location)
{
    auto open = location.rfind('(');

    if (open == std::string_view::npos)
        return false;

    auto inside = location.substr(open + 1);
    auto comma = inside.find(',');

    if (comma == std::string_view::npos)
        return isNumber(inside);

    return isNumber(inside.substr(0, comma)) && isNumber(inside.substr(comma + 1));
}

bool isLocatedCompilerError(std::string_view line)
{
    for (auto marker: {": error:", ": fatal error:"})
        if (auto at = line.find(marker); at != std::string_view::npos)
            if (endsWithLineAndColumn(line.substr(0, at)))
                return true;

    for (auto marker: {"): error ", "): fatal error "})
        if (auto at = line.find(marker); at != std::string_view::npos)
            if (endsWithParenthesisedLine(line.substr(0, at)))
                return true;

    return false;
}

bool isDriverError(std::string_view line)
{
    return line.find("collect2: error:") != std::string_view::npos
           || line.find("linker command failed") != std::string_view::npos
           || line.find("ninja: error:") != std::string_view::npos;
}

bool isOtherError(std::string_view line)
{
    return line.find("error:") != std::string_view::npos && !isDriverError(line);
}

bool isCMakeError(std::string_view line)
{
    return line.find("CMake Error") != std::string_view::npos;
}

bool isNinjaFailure(std::string_view line)
{
    return line.starts_with("FAILED:");
}

std::string summaryOf(std::string_view output)
{
    auto lines = nonEmptyLines(output);

    for (auto matches:
         {isLocatedCompilerError, isOtherError, isCMakeError, isNinjaFailure})
        for (auto line: lines)
            if (matches(line))
                return std::string {line};

    return lines.empty() ? std::string {} : std::string {lines.back()};
}

std::string summaryWithoutOutput(const Processes::ProcessResult& result)
{
    if (!result.launched)
        return "build command did not start";

    if (!result.exited)
        return "build cancelled";

    return "build failed with exit code " + std::to_string(result.exitCode);
}

bool succeeded(const Processes::ProcessResult& result)
{
    return result.exited && result.exitCode == 0;
}

bool isInside(const FilePath& path, const FilePath& directory)
{
    auto prefix = directory.str();

    if (!prefix.ends_with('/'))
        prefix += '/';

    return (path.str() + "/").starts_with(prefix);
}

HotReloadLoop::Options hotReloadOptionsFor(const HotReloadTarget& target,
                                           const std::string& requiredSymbol)
{
    auto options = HotReloadLoop::Options {};
    options.sources.directory = FilePath {target.sourceDirectory};
    options.buildCommand = hotReloadBuildCommand(target);
    options.modulePath = FilePath {target.library};
    options.requiredSymbol = requiredSymbol;
    return options;
}

Files::SourceWatcher::Options
    sourcesExcludingOutputs(const HotReloadLoop::Options& options)
{
    auto sources = options.sources;
    auto& excluded = sources.watcher.excludedDirectories;

    for (const auto& output:
         {options.modulePath.parentDirectory(), options.stagingDirectory})
        if (isInside(output, sources.directory))
            excluded.add(output);

    return sources;
}
} // namespace

BuildFailure buildFailureFrom(const Processes::ProcessResult& result)
{
    auto failure = BuildFailure {};
    failure.output = result.output;
    failure.launched = result.launched;
    failure.exited = result.exited;
    failure.exitCode = result.exitCode;

    auto summary = summaryOf(withoutAnsiEscapes(result.output));
    failure.summary = summary.empty() ? summaryWithoutOutput(result) : summary;
    return failure;
}

HotReloadLoop::HotReloadLoop(const Options& options)
    : library(options.modulePath, options.stagingDirectory, options.requiredSymbol)
    , runner(options.buildCommand)
    , watcher(sourcesExcludingOutputs(options), rebuildOnChange())
{
    auto forwardOutput = [this](std::string_view chunk) { onBuildOutput(chunk); };
    auto finished = [this](const Processes::ProcessResult& result)
    { buildFinished(result); };
    runner.onOutput = forwardOutput;
    runner.onFinished = finished;
}

HotReloadLoop::HotReloadLoop(const HotReloadTarget& target,
                             const std::string& requiredSymbol)
    : HotReloadLoop(hotReloadOptionsFor(target, requiredSymbol))
{
}

Files::FileWatcher::ChangedFunc HotReloadLoop::rebuildOnChange()
{
    auto changed = [this](const Vector<FilePath>&) { rebuild(); };
    return changed;
}

void HotReloadLoop::rebuild()
{
    if (!runner.isRunning())
        startBuilding();

    runner.run();
}

void HotReloadLoop::startBuilding()
{
    setState(State::Building);
    onBuildStarted();
}

void HotReloadLoop::buildFinished(const Processes::ProcessResult& result)
{
    if (succeeded(result))
    {
        reload();
    }
    else
    {
        auto buildFailure = buildFailureFrom(result);
        failureMessage = buildFailure.summary;
        setState(State::BuildFailed);
        onBuildFailed(buildFailure);
    }

    if (runner.isRerunPending())
        startBuilding();
}

void HotReloadLoop::reload()
{
    if (!library.reload())
    {
        failureMessage = library.lastError();
        setState(State::LoadFailed);
        onLoadFailed(failureMessage);
        return;
    }

    failureMessage.clear();
    setState(State::Loaded);
    onReloaded(library);
}

void HotReloadLoop::setState(State newState)
{
    state = newState;
    onStateChanged();
}

HotReloadLoop::State HotReloadLoop::getState() const
{
    return state;
}

std::string HotReloadLoop::describeState() const
{
    switch (state)
    {
        case State::Building:
            return "building…";
        case State::BuildFailed:
            return "build failed: " + failureMessage;
        case State::LoadFailed:
            return "load failed: " + failureMessage;
        case State::Idle:
        case State::Loaded:
            break;
    }

    return "generation " + std::to_string(getGeneration());
}

int HotReloadLoop::getGeneration() const
{
    return library.getGeneration();
}

ReloadableLibrary& HotReloadLoop::getLibrary()
{
    return library;
}

const FilePath& HotReloadLoop::getSourceDirectory() const
{
    return watcher.getDirectory();
}
} // namespace eacp::Plugins
