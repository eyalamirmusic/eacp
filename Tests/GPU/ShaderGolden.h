#pragma once

#include "CodegenCommon.h"

#include <eacp/GPU/Codegen/ShaderProgram.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Every shader a module ships, emitted in all three dialects and compared byte
// for byte with the text checked in beside the test. The corpus is a list: an
// entry names the files, and the directory may hold nothing the list does not
// produce. EACP_UPDATE_GOLDENS=1 rewrites the files instead, and deletes the
// ones no entry produces any more.

namespace eacp::ShaderGolden
{
struct Emitted
{
    std::string msl;
    std::string hlsl;
    std::string glsl;
    bool isCompute = false;
};

using Emit = std::function<Emitted()>;
using GraphWalk = std::function<void(const GPU::ShaderGraphVisitor&)>;

struct Entry
{
    std::string name;
    Emit emit = [] { return Emitted {}; };
};

struct Dialect
{
    const char* extension;
    std::string Emitted::* text;
};

inline const std::vector<Dialect>& dialects()
{
    static const auto all = std::vector<Dialect> {
        {"msl", &Emitted::msl},
        {"hlsl", &Emitted::hlsl},
        {"glsl", &Emitted::glsl},
    };

    return all;
}

inline Emitted emitAll(const GPU::ShaderGraph& graph)
{
    return {GPU::emitMetal(graph),
            GPU::emitHlsl(graph),
            GPU::emitGlsl(graph),
            graph.isCompute()};
}

template <typename Program, typename... Args>
Emit program(Args... arguments)
{
    return [=]
    {
        auto built = Program {arguments...};
        return emitAll(built.graph());
    };
}

// The index-th graph a module hands over through its forEachShaderGraph, for
// the programs it keeps private to its own translation unit.
inline Emit walked(GraphWalk walk, int index)
{
    return [=]
    {
        auto result = Emitted {};
        auto seen = 0;

        walk(
            [&](const GPU::ShaderGraph& graph)
            {
                if (seen++ == index)
                    result = emitAll(graph);
            });

        nano::check(index < seen, "the walk hands over fewer graphs than listed");
        return result;
    };
}

inline bool isUpdating()
{
    const auto* value = std::getenv("EACP_UPDATE_GOLDENS");
    return value != nullptr && std::string(value) == "1";
}

inline std::string readFile(const std::filesystem::path& path)
{
    auto stream = std::ifstream(path, std::ios::binary);
    auto contents = std::ostringstream {};
    contents << stream.rdbuf();
    return contents.str();
}

inline void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

inline std::vector<std::string> splitLines(const std::string& text)
{
    auto lines = std::vector<std::string> {};
    auto stream = std::istringstream(text);

    for (auto line = std::string {}; std::getline(stream, line);)
        lines.push_back(line);

    return lines;
}

// A unified-diff hunk around the first run of lines the two disagree on: the
// run ends where the two tails agree again, each side is cut at eight lines,
// and three lines of context stand either side of it.
inline std::string firstDifference(const std::string& path,
                                   const std::string& expected,
                                   const std::string& actual)
{
    constexpr auto context = size_t {3};
    constexpr auto shown = size_t {8};

    const auto before = splitLines(expected);
    const auto after = splitLines(actual);

    auto first = size_t {0};

    while (first < before.size() && first < after.size()
           && before[first] == after[first])
        ++first;

    auto endBefore = before.size();
    auto endAfter = after.size();

    while (endBefore > first && endAfter > first
           && before[endBefore - 1] == after[endAfter - 1])
    {
        --endBefore;
        --endAfter;
    }

    const auto start = first > context ? first - context : 0;
    const auto removed = std::min(endBefore - first, shown);
    const auto added = std::min(endAfter - first, shown);
    const auto trailing = removed == endBefore - first && added == endAfter - first
                              ? std::min(before.size() - endBefore, context)
                              : 0;

    auto hunk = std::ostringstream {};
    hunk << "--- " << path << "\n+++ " << path << " (emitted)\n"
         << "@@ -" << start + 1 << "," << first - start + removed + trailing << " +"
         << start + 1 << "," << first - start + added + trailing << " @@\n";

    for (auto line = start; line < first; ++line)
        hunk << " " << before[line] << "\n";

    for (auto line = first; line < first + removed; ++line)
        hunk << "-" << before[line] << "\n";

    for (auto line = first; line < first + added; ++line)
        hunk << "+" << after[line] << "\n";

    for (auto line = endBefore; line < endBefore + trailing; ++line)
        hunk << " " << before[line] << "\n";

    hunk << "(EACP_UPDATE_GOLDENS=1 rewrites the golden)";
    return hunk.str();
}

inline std::string fileName(const Entry& entry, const Dialect& dialect)
{
    return entry.name + "." + dialect.extension;
}

inline void expectMatchesGolden(const std::filesystem::path& path,
                                const std::string& emitted)
{
    const auto exists = std::filesystem::exists(path);

    if (isUpdating())
    {
        if (!exists || readFile(path) != emitted)
        {
            writeFile(path, emitted);
            std::cout << (exists ? "rewrote " : "wrote ") << path.string() << "\n";
        }

        return;
    }

    if (!exists)
    {
        nano::check(false,
                    "no golden at " + path.string()
                        + " (EACP_UPDATE_GOLDENS=1 writes it)");
        return;
    }

    const auto golden = readFile(path);

    if (golden != emitted)
        nano::check(false, firstDifference(path.string(), golden, emitted));
}

inline void checkEntry(const std::filesystem::path& directory, const Entry& entry)
{
    const auto emitted = entry.emit();

    for (const auto& dialect: dialects())
        expectMatchesGolden(directory / fileName(entry, dialect),
                            emitted.*dialect.text);

    expectGlslCompiles(emitted.glsl, emitted.isCompute);
}

inline std::set<std::string> expectedFiles(const std::vector<Entry>& entries)
{
    auto files = std::set<std::string> {};

    for (const auto& entry: entries)
        for (const auto& dialect: dialects())
            files.insert(fileName(entry, dialect));

    return files;
}

inline std::vector<std::string> filesUnder(const std::filesystem::path& directory)
{
    auto files = std::vector<std::string> {};

    if (!std::filesystem::exists(directory))
        return files;

    for (const auto& item: std::filesystem::recursive_directory_iterator(directory))
    {
        if (!item.is_regular_file())
            continue;

        files.push_back(
            std::filesystem::relative(item.path(), directory).generic_string());
    }

    return files;
}

// A golden no entry produces is a kernel that was deleted or renamed without
// its files, and it would otherwise sit there checking nothing.
inline void expectNoOrphans(const std::filesystem::path& directory,
                            const std::vector<Entry>& entries)
{
    const auto expected = expectedFiles(entries);

    nano::check(expected.size() == entries.size() * dialects().size(),
                "two entries share a name");

    for (const auto& file: filesUnder(directory))
    {
        if (expected.contains(file))
            continue;

        if (isUpdating())
        {
            std::filesystem::remove(directory / file);
            std::cout << "deleted " << (directory / file).string() << "\n";
        }
        else
            nano::check(false,
                        "golden " + file
                            + " is produced by no entry (EACP_UPDATE_GOLDENS=1 "
                              "deletes it)");
    }
}

inline bool registerCorpus(const std::string& suite,
                           const std::filesystem::path& directory,
                           const std::vector<Entry>& entries)
{
    for (const auto& entry: entries)
        nano::test(suite + "/" + entry.name,
                   [directory, entry] { checkEntry(directory, entry); });

    nano::test(suite + "/noOrphans",
               [directory, entries] { expectNoOrphans(directory, entries); });

    return true;
}
} // namespace eacp::ShaderGolden
