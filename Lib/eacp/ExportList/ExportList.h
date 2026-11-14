#pragma once

#include "Archive.h"
#include "Coff.h"

#include <eacp/Core/Utils/FilePath.h>

#include <string>
#include "NameIndex.h"

// The symbols a hot-reload module may import from the app's executable: every
// external symbol the app's own objects and its static closure define, less
// what the module compiles a copy of itself. Lib/eacp/ExportList/README.md is
// the account of what is listed and what is dropped.
namespace eacp::ExportList
{
enum class Filter
{
    // The std rule plus COMDAT data: what a module can need from the exe.
    HotReload,
    // Only the exclusions CMake's __create_def applies, for comparison.
    Unfiltered,
};

enum class Verdict
{
    Export,
    ExportData,
    ExcludedByName,
    ReadOnlyData,
    ComdatData,
    StandardLibrary,
};

// What every definition of one name, across all the inputs, said about it.
struct SymbolFacts
{
    SymbolKind kind = SymbolKind::Code;
    bool comdat = true;
};

// The names CMake's __create_def never exports: deleting destructors, names
// with a dot, C++/CLI thunks and operator-only C names.
bool isExcludedName(std::string_view name);

// Whether a decorated name is an entity of namespace std itself (its
// outermost scope), as dbghelp's undecorator reads it. Not thread-safe.
bool isInStandardNamespace(std::string_view decoratedName);

// The qualified name dbghelp reads from a decorated one, without its type, or
// the name itself when it is not decorated. Not thread-safe.
std::string undecorate(std::string_view decoratedName);

Verdict
    classifySymbol(std::string_view name, const SymbolFacts& facts, Filter filter);

struct Export
{
    std::string name;
    bool data = false;

    bool operator==(const Export&) const = default;
};

struct Statistics
{
    std::size_t files = 0;
    std::size_t objects = 0;
    std::size_t definitions = 0;
    std::size_t uniqueNames = 0;
    std::size_t excludedByName = 0;
    std::size_t readOnlyData = 0;
    std::size_t comdatData = 0;
    std::size_t standardLibrary = 0;
    std::size_t exported = 0;
};

// Gathers the defined externals of objects and archives. Every input must be
// for one machine.
class Collector
{
public:
    // An object or an archive, told apart by its first bytes.
    void addFile(const FilePath& path);
    void addBytes(Bytes bytes, const FilePath& displayPath);

    Machine machine() const { return inputMachine; }

    // Sorted by name, so the same inputs always give the same list.
    Vector<Export> exports(Filter filter, Statistics* statistics = nullptr) const;

    // Every defined name once, in no particular order.
    void forEachSymbol(
        const std::function<void(std::string_view, const SymbolFacts&)>& visit) const
    {
        for (const auto& entry: symbols.all())
            visit(entry.name, entry.value);
    }

private:
    void addObject(Bytes bytes, std::string_view displayName);
    void addArchive(Bytes bytes, const FilePath& path);
    void record(const DefinedSymbol& symbol);
    void checkMachine(Machine machine, std::string_view displayName);

    NameIndex<SymbolFacts> symbols;
    Machine inputMachine = Machine::Unknown;
    std::size_t fileCount = 0;
    std::size_t objectCount = 0;
    std::size_t definitionCount = 0;
};

// The module-definition file: NAME, so an import library made from it names
// the image even under llvm-lib, which ignores /name:, and EXPORTS.
std::string makeDefinitionFile(std::string_view imageName,
                               const Vector<Export>& exports);

// Writes text to path unless the file already holds exactly that, so an
// unchanged list leaves its timestamp, and everything linked from it, alone.
// Answers whether it wrote.
bool writeIfChanged(const FilePath& path, std::string_view text);

// A PE image's export table holds at most 65,535 entries.
constexpr auto maximumExports = std::size_t {65535};

struct ExportLimits
{
    std::size_t warnAt = maximumExports / 4 * 3;
    std::size_t maximum = maximumExports;
};

enum class LimitStatus
{
    WithinLimit,
    NearLimit,
    OverLimit,
};

LimitStatus checkLimit(std::size_t count, const ExportLimits& limits = {});
} // namespace eacp::ExportList
