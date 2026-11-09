#include "Reloadable.h"

#include <stdexcept>

namespace eacp::Plugins
{
std::string Detail::readableSignature(std::string_view signature)
{
    constexpr auto marker = std::string_view {"Signature = "};
    auto start = signature.find(marker);

    if (start != std::string_view::npos)
    {
        signature.remove_prefix(start + marker.size());

        if (signature.ends_with(']'))
            signature.remove_suffix(1);
    }

    return std::string {signature};
}

namespace
{
using ReloadableTableFunction = ReloadableTable (*)();

ReloadableEntry::Erased findReloadable(const ReloadableTable& table,
                                       std::string_view signature)
{
    for (auto index = 0; index < table.count; ++index)
    {
        const auto& entry = table.entries[index];

        if (entry.function != nullptr && signature == entry.signature)
            return entry.function;
    }

    return nullptr;
}

Vector<std::string> reloadableSignaturesOf(const ReloadableTable& table)
{
    auto signatures = Vector<std::string> {};

    for (auto index = 0; index < table.count; ++index)
        if (table.entries[index].function != nullptr)
            signatures.add(table.entries[index].signature);

    return signatures;
}

std::string joinedSignatures(const Vector<std::string>& signatures)
{
    auto text = std::string {};

    for (const auto& signature: signatures)
        text += (text.empty() ? "" : ", ") + Detail::readableSignature(signature);

    return text;
}

std::string reloadRefusalFor(const Vector<std::string>& expected,
                             const ReloadableTable& module)
{
    auto missing = Vector<std::string> {};

    for (const auto& signature: expected)
        if (findReloadable(module, signature) == nullptr)
            missing.add(signature);

    if (missing.empty())
        return {};

    auto extra = Vector<std::string> {};

    for (const auto& signature: reloadableSignaturesOf(module))
        if (!expected.contains(signature))
            extra.add(signature);

    if (extra.empty())
        return "the module lacks " + joinedSignatures(missing);

    return "signature mismatch: the module lacks " + joinedSignatures(missing)
           + " and has " + joinedSignatures(extra);
}

std::string watchedPathsText(const Vector<FilePath>& paths)
{
    auto text = std::string {};

    for (const auto& path: paths)
        text += (text.empty() ? "" : ", ") + path.str();

    return text;
}

HotReloadLoop::Options
    reloadableTargetOptions(const HotReloadTarget& target,
                            const Files::SourceWatcher::Options& sources)
{
    auto options = hotReloadOptions(target);
    auto watched = sources;

    if (watched.directory.empty())
        watched.directory = options.sources.directory;

    if (watched.files.empty())
        watched.files = options.sources.files;

    options.sources = watched;
    return options;
}
} // namespace

Reloadable::Reloadable(const Detail::ReloadableSetup& setup,
                       const ReloadableOptions& optionsToUse)
    : options(optionsToUse)
    , compiledIn(setup.compiledIn)
{
    if (setup.hotReload)
        startLoop(setup.target);

    if (options.callOnReloadedAtCreation)
        options.onReloaded();
}

Reloadable::~Reloadable() = default;

std::string Reloadable::describeState() const
{
    if (!loop)
        return "static";

    auto state = loop->getState();

    if (state != HotReloadLoop::State::Idle && state != HotReloadLoop::State::Loaded)
        return loop->describeState();

    if (!imageLoaded)
        return "load failed: " + firstLoadFailure;

    return "generation " + std::to_string(getGeneration());
}

int Reloadable::getGeneration() const
{
    if (!loop || !imageLoaded)
        return 0;

    return loop->getGeneration() - firstImageGeneration;
}

ReloadableEntry::Erased Reloadable::lookup(std::string_view signature) const
{
    if (loop && !imageLoaded)
        return nullptr;

    if (auto function = findReloadable(loop ? loaded : compiledIn, signature))
        return function;

    throw std::logic_error("EACP_RELOADABLE lists no "
                           + Detail::readableSignature(signature));
}

void Reloadable::logFactoryFailure(const std::string& reason) const
{
    options.log("HotReload: " + reason);
}

std::string Reloadable::refusalOf(const DynamicLibrary& image) const
{
    auto table = image.findFunction<ReloadableTableFunction>(reloadableTableSymbol);

    if (table == nullptr)
        return std::string {"no "} + reloadableTableSymbol;

    if (!imageLoaded)
        return {};

    return reloadRefusalFor(firstSignatures, table());
}

HotReloadLoop::Options Reloadable::loopOptions(const HotReloadTarget& target) const
{
    auto loopOptions = options.loop
                           ? *options.loop
                           : reloadableTargetOptions(target, options.sources);
    auto validateAlso = loopOptions.validate;

    auto validate = [this, validateAlso](const DynamicLibrary& image)
    {
        auto refusal = refusalOf(image);
        return refusal.empty() ? validateAlso(image) : refusal;
    };

    loopOptions.requiredSymbol = reloadableTableSymbol;
    loopOptions.validate = validate;
    return loopOptions;
}

void Reloadable::startLoop(const HotReloadTarget& target)
{
    auto& running = loop.emplace(loopOptions(target));

    auto stateChanged = [this] { options.onStateChanged(); };

    auto buildStarted = [this]
    {
        options.log("HotReload: building " + loop->getLibrary().getBuiltPath().stem()
                    + "…");
    };

    auto buildOutput = [this](std::string_view chunk)
    { options.onBuildOutput(chunk); };

    auto buildFailed = [this](const BuildFailure& failure)
    {
        options.log("HotReload: build failed: " + failure.summary);
        options.onBuildFailed(failure);
    };

    auto loadFailed = [this](const std::string& error)
    {
        options.log("HotReload: load failed: " + error);
        options.onLoadFailed(error);
    };

    auto swapped = [this](ReloadableLibrary&) { reloaded(); };

    running.onStateChanged = stateChanged;
    running.onBuildStarted = buildStarted;
    running.onBuildOutput = buildOutput;
    running.onBuildFailed = buildFailed;
    running.onLoadFailed = loadFailed;
    running.onReloaded = swapped;

    options.log("HotReload: watching "
                + watchedPathsText(running.getWatchedPaths()));
    loadFirstImage();
}

void Reloadable::loadFirstImage()
{
    auto& library = loop->getLibrary();

    if (library.reload())
    {
        useLoadedImage();
        options.log("HotReload: generation 0");
    }
    else
    {
        firstLoadFailure = library.lastError();
        options.log("HotReload: load failed: " + firstLoadFailure);
        options.onLoadFailed(firstLoadFailure);
    }

    options.onStateChanged();
}

void Reloadable::useLoadedImage()
{
    auto& library = loop->getLibrary();
    loaded = library.findFunction<ReloadableTableFunction>(reloadableTableSymbol)();

    if (imageLoaded)
        return;

    imageLoaded = true;
    firstImageGeneration = library.getGeneration();
    firstSignatures = reloadableSignaturesOf(loaded);
}

void Reloadable::reloaded()
{
    useLoadedImage();

    options.log("HotReload: generation " + std::to_string(getGeneration()));
    options.onReloaded();
}
} // namespace eacp::Plugins
