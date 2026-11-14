#pragma once

#include <eacp/Core/Core.h>
#include <eacp/ExportList/ExportList.h>
#include <NanoTest/NanoTest.h>

#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

namespace ExportListTesting
{
using namespace eacp;
using namespace eacp::ExportList;

inline Vector<Export> exportsOf(std::initializer_list<FilePath> files,
                                Filter filter = Filter::HotReload,
                                Statistics* statistics = nullptr)
{
    auto collector = Collector {};

    for (const auto& file: files)
        collector.addFile(file);

    return collector.exports(filter, statistics);
}

inline const Export* findExport(const Vector<Export>& exports, std::string_view name)
{
    for (const auto& entry: exports)
        if (entry.name == name)
            return &entry;

    return nullptr;
}

inline const Export* findExportMatching(const Vector<Export>& exports,
                                        std::string_view prefix,
                                        std::string_view suffix = {})
{
    for (const auto& entry: exports)
        if (entry.name.starts_with(prefix) && entry.name.ends_with(suffix))
            return &entry;

    return nullptr;
}

inline bool anyExportStartsWith(const Vector<Export>& exports,
                                std::string_view prefix)
{
    return findExportMatching(exports, prefix) != nullptr;
}

// The message of the Error body throws, or empty when it throws none.
inline std::string errorFrom(const std::function<void()>& body)
{
    try
    {
        body();
    }
    catch (const Error& error)
    {
        return error.what();
    }

    return {};
}

inline bool mentionsAll(std::string_view text,
                        std::initializer_list<std::string_view> fragments)
{
    if (text.empty())
        return false;

    for (auto fragment: fragments)
        if (text.find(fragment) == std::string_view::npos)
            return false;

    return true;
}

inline Bytes bytesOf(const std::string& text)
{
    return Bytes {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// A COFF header for machine with no sections and no symbols: an empty object.
inline std::string emptyObject(std::uint16_t machine)
{
    auto bytes = std::string(20, '\0');
    bytes[0] = static_cast<char>(machine & 0xFF);
    bytes[1] = static_cast<char>(machine >> 8);
    return bytes;
}

namespace Symbols
{
constexpr auto plainFunction = "?plainFunction@ExportListFixture@@YAHH@Z";
constexpr auto writableGlobal = "?writableGlobal@ExportListFixture@@3HA";
constexpr auto inlineVariable = "?inlineCounter@ExportListFixture@@3HA";
constexpr auto inlineFunction = "?inlineFunction@ExportListFixture@@YAHH@Z";
constexpr auto explicitInstantiation = "??$twice@H@ExportListFixture@@YAHH@Z";
constexpr auto archiveOnly = "?archiveOnlyFunction@ExportListFixture@@YAHXZ";
constexpr auto vftable = "??_7Shape@ExportListFixture@@6B@";
constexpr auto standardToStringPrefix = "?to_string@std@@";
constexpr auto standardToStringSuffix = "@H@Z";
constexpr auto standardHashCallPrefix = "??R?$hash@UKey@ExportListFixture@@@std@@";
constexpr auto deletingDestructorPrefix = "??_G";
constexpr auto stringLiteralPrefix = "??_C@";
} // namespace Symbols
} // namespace ExportListTesting
