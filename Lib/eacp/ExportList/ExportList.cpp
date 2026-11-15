#include "ExportList.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/MemoryMappedFile.h>
#include <eacp/Core/Utils/Strings.h>

#include <algorithm>

namespace eacp::ExportList
{
namespace
{
constexpr auto operatorCharacters = std::string_view {"=<>+-*/%,?|~!^&[]()"};
constexpr auto definitionSpecialCharacters = std::string_view {" \t;="};

bool isDeletingDestructor(std::string_view name)
{
    return name.starts_with("??_G") || name.starts_with("??_E");
}

bool isManagedCode(std::string_view name)
{
    return name == "__t2m" || name == "__m2mep" || name == "__mep"
           || name.find("$$F") != std::string_view::npos
           || name.find("$$J") != std::string_view::npos;
}

bool isOperatorOnly(std::string_view name)
{
    return name.find_first_not_of(operatorCharacters) == std::string_view::npos;
}

bool isVftable(std::string_view name)
{
    return name.starts_with("??_7");
}

std::string quotedIfNeeded(std::string_view name)
{
    if (name.find_first_of(definitionSpecialCharacters) == std::string_view::npos)
        return std::string {name};

    return Strings::concat("\"", name, "\"");
}

struct Kept
{
    std::string_view name;
    bool data = false;
};

void count(Verdict verdict, Statistics& statistics)
{
    switch (verdict)
    {
        case Verdict::Export:
        case Verdict::ExportData:
            ++statistics.exported;
            break;
        case Verdict::ExcludedByName:
            ++statistics.excludedByName;
            break;
        case Verdict::ReadOnlyData:
            ++statistics.readOnlyData;
            break;
        case Verdict::ComdatData:
            ++statistics.comdatData;
            break;
        case Verdict::StandardLibrary:
            ++statistics.standardLibrary;
            break;
    }
}

Verdict unfilteredVerdict(std::string_view name, const SymbolFacts& facts)
{
    switch (facts.kind)
    {
        case SymbolKind::Code:
            return Verdict::Export;
        case SymbolKind::Data:
            return Verdict::ExportData;
        case SymbolKind::ReadOnlyData:
            break;
    }

    return isVftable(name) ? Verdict::ExportData : Verdict::ReadOnlyData;
}
} // namespace

bool isExcludedName(std::string_view name)
{
    return name.empty() || isDeletingDestructor(name)
           || name.find('.') != std::string_view::npos || isManagedCode(name)
           || isOperatorOnly(name);
}

Verdict
    classifySymbol(std::string_view name, const SymbolFacts& facts, Filter filter)
{
    if (isExcludedName(name))
        return Verdict::ExcludedByName;

    if (filter == Filter::Unfiltered)
        return unfilteredVerdict(name, facts);

    if (facts.comdat && facts.kind != SymbolKind::Code)
        return Verdict::ComdatData;

    if (facts.kind == SymbolKind::ReadOnlyData)
        return Verdict::ReadOnlyData;

    if (facts.comdat && isInStandardNamespace(name))
        return Verdict::StandardLibrary;

    return facts.kind == SymbolKind::Data ? Verdict::ExportData : Verdict::Export;
}

void Collector::addFile(const FilePath& path)
{
    auto file = MemoryMappedFile {path};

    if (!file.isValid())
        throw Error(Strings::concat(path.str(), ": cannot be read"));

    addBytes(file.bytes(), path);
}

void Collector::addBytes(Bytes bytes, const FilePath& displayPath)
{
    ++fileCount;

    if (isArchive(bytes))
        addArchive(bytes, displayPath);
    else
        addObject(bytes, displayPath.str());
}

void Collector::addArchive(Bytes bytes, const FilePath& path)
{
    auto archiveName = path.str();

    forEachArchiveMember(bytes,
                         path,
                         [&](const ArchiveMember& member)
                         {
                             auto displayName =
                                 Strings::concat(archiveName, "(", member.name, ")");
                             addObject(member.bytes, displayName);
                         });
}

void Collector::addObject(Bytes bytes, std::string_view displayName)
{
    ++objectCount;

    auto machine = readObjectSymbols(
        bytes, displayName, [this](const DefinedSymbol& symbol) { record(symbol); });

    checkMachine(machine, displayName);
}

void Collector::checkMachine(Machine machine, std::string_view displayName)
{
    if (machine == Machine::Unknown)
        return;

    if (inputMachine == Machine::Unknown)
    {
        inputMachine = machine;
        return;
    }

    if (machine != inputMachine)
        throw Error(Strings::concat(displayName,
                                    ": is for ",
                                    machineName(machine),
                                    " but earlier inputs are for ",
                                    machineName(inputMachine)));
}

void Collector::record(const DefinedSymbol& symbol)
{
    ++definitionCount;

    auto added = false;
    auto& facts = symbols.findOrAdd(
        symbol.name, SymbolFacts {symbol.kind, symbol.comdat}, added);

    if (!added)
        facts.comdat = facts.comdat && symbol.comdat;
}

Vector<Export> Collector::exports(Filter filter, Statistics* statistics) const
{
    auto kept = std::vector<Kept> {};
    auto tally = Statistics {};
    tally.files = fileCount;
    tally.objects = objectCount;
    tally.definitions = definitionCount;
    tally.uniqueNames = symbols.size();

    for (const auto& entry: symbols.all())
    {
        auto verdict = classifySymbol(entry.name, entry.value, filter);
        count(verdict, tally);

        if (verdict == Verdict::Export || verdict == Verdict::ExportData)
            kept.push_back({entry.name, verdict == Verdict::ExportData});
    }

    std::sort(kept.begin(),
              kept.end(),
              [](const Kept& left, const Kept& right)
              { return left.name < right.name; });

    auto result = Vector<Export> {};
    result.reserve(static_cast<int>(kept.size()));

    for (const auto& entry: kept)
        result.add(Export {std::string {entry.name}, entry.data});

    if (statistics != nullptr)
        *statistics = tally;

    return result;
}

std::string makeDefinitionFile(std::string_view imageName,
                               const Vector<Export>& exports)
{
    auto text = Strings::concat("NAME ", quotedIfNeeded(imageName), "\nEXPORTS\n");

    for (const auto& entry: exports)
    {
        text += "    ";
        text += quotedIfNeeded(entry.name);
        text += entry.data ? " DATA\n" : "\n";
    }

    return text;
}

bool writeIfChanged(const FilePath& path, std::string_view text)
{
    if (Files::isFile(path) && Files::readFile(path) == text)
        return false;

    if (auto directory = path.parentDirectory(); !directory.str().empty())
        Files::createDirectories(directory);

    Files::writeFileAtomically(path, text);
    return true;
}

LimitStatus checkLimit(std::size_t count, const ExportLimits& limits)
{
    if (count > limits.maximum)
        return LimitStatus::OverLimit;

    if (count >= limits.warnAt)
        return LimitStatus::NearLimit;

    return LimitStatus::WithinLimit;
}
} // namespace eacp::ExportList
