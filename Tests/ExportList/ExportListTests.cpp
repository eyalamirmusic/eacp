#include "Common.h"

#include <eacp/Core/Utils/MemoryMappedFile.h>
#include <eacp/Core/Utils/StdPath.h>
#include <eacp/Core/Utils/TemporaryFile.h>

#include <filesystem>

using namespace nano;
using namespace ExportListTesting;

namespace ExportListLibraryTests
{
std::string fileBytes(const FilePath& path)
{
    return Files::readFile(path);
}

Machine machineOf(const FilePath& path)
{
    auto collector = Collector {};
    collector.addFile(path);
    return collector.machine();
}

auto tReadsClassicObject = test("ExportList/readsAClassicObject") = []
{
    auto bytes = fileBytes(EXPORT_LIST_CLASSIC);
    check(detectObjectFormat(bytesOf(bytes)) == ObjectFormat::Coff);

    auto exports = exportsOf({EXPORT_LIST_CLASSIC});
    check(findExport(exports, Symbols::plainFunction) != nullptr);
    check(machineOf(EXPORT_LIST_CLASSIC) != Machine::Unknown);
};

// clang-cl accepts /bigobj but writes the extended header only when an object
// needs more sections than the classic one counts, so there the fixture is a
// classic object and only cl's is checked for its format.
auto tReadsBigObj = test("ExportList/readsABigObjObjectAsTheClassicOne") = []
{
    auto bytes = fileBytes(EXPORT_LIST_BIGOBJ);

#ifdef __clang__
    Console::print("ExportList/readsABigObjObjectAsTheClassicOne: clang-cl wrote a "
                   "classic object; its format is checked under cl");
#else
    check(detectObjectFormat(bytesOf(bytes)) == ObjectFormat::BigObj);
#endif

    for (auto filter: {Filter::HotReload, Filter::Unfiltered})
    {
        auto classic = exportsOf({EXPORT_LIST_CLASSIC}, filter);
        auto bigObj = exportsOf({EXPORT_LIST_BIGOBJ}, filter);
        check(!bigObj.empty());
        check(bigObj == classic);
    }
};

auto tReadsArchive = test("ExportList/readsEveryMemberOfAnArchive") = []
{
    auto statistics = Statistics {};
    auto exports = exportsOf({EXPORT_LIST_ARCHIVE}, Filter::HotReload, &statistics);

    check(statistics.files == 1);
    check(statistics.objects == 2);
    check(findExport(exports, Symbols::plainFunction) != nullptr);
    check(findExport(exports, Symbols::archiveOnly) != nullptr);
};

auto tReadsThinArchive = test("ExportList/readsAThinArchive") = []
{
#ifdef EXPORT_LIST_THIN_ARCHIVE
    auto bytes = fileBytes(EXPORT_LIST_THIN_ARCHIVE);
    check(bytes.starts_with("!<thin>\n"));

    auto statistics = Statistics {};
    auto thin =
        exportsOf({EXPORT_LIST_THIN_ARCHIVE}, Filter::HotReload, &statistics);
    check(statistics.objects == 1);
    check(thin == exportsOf({EXPORT_LIST_CLASSIC}));
#else
    Console::print("ExportList/readsAThinArchive: skipped, no llvm-lib to make one");
#endif
};

auto tComdatFunctionVsData =
    test("ExportList/keepsComdatFunctionsDropsComdatData") = []
{
    auto statistics = Statistics {};
    auto exports = exportsOf({EXPORT_LIST_CLASSIC}, Filter::HotReload, &statistics);

    check(findExport(exports, Symbols::inlineFunction) != nullptr);
    check(findExport(exports, Symbols::inlineVariable) == nullptr);
    check(findExport(exports, Symbols::vftable) == nullptr);
    check(statistics.comdatData >= 2);

    auto global = findExport(exports, Symbols::writableGlobal);
    check(global != nullptr && global->data);

    auto unfiltered = exportsOf({EXPORT_LIST_CLASSIC}, Filter::Unfiltered);
    auto inlineVariable = findExport(unfiltered, Symbols::inlineVariable);
    check(inlineVariable != nullptr && inlineVariable->data);
};

auto tKeepsExplicitInstantiation =
    test("ExportList/keepsAnExplicitInstantiation") = []
{
    for (auto file: {EXPORT_LIST_CLASSIC, EXPORT_LIST_GY})
    {
        auto exports = exportsOf({file});
        auto instantiation = findExport(exports, Symbols::explicitInstantiation);
        check(instantiation != nullptr && !instantiation->data);
    }
};

auto tDropsStandardComdat = test("ExportList/dropsAnInlineStdFunction") = []
{
    auto statistics = Statistics {};
    auto filtered = exportsOf({EXPORT_LIST_CLASSIC}, Filter::HotReload, &statistics);
    auto unfiltered = exportsOf({EXPORT_LIST_CLASSIC}, Filter::Unfiltered);

    check(findExportMatching(unfiltered,
                             Symbols::standardToStringPrefix,
                             Symbols::standardToStringSuffix)
          != nullptr);
    check(findExportMatching(filtered,
                             Symbols::standardToStringPrefix,
                             Symbols::standardToStringSuffix)
          == nullptr);
    check(statistics.standardLibrary >= 1);
};

// An explicit specialisation of a std template defined out of line is an
// ordinary definition, in one object only: a module calls it, never copies it.
// Under /Gy it sits in a no-duplicates COMDAT and must still be kept.
auto tKeepsStandardSpecialisation =
    test("ExportList/keepsAStdSpecialisationDefinedOutOfLine") = []
{
    for (auto file: {EXPORT_LIST_CLASSIC, EXPORT_LIST_GY})
    {
        auto exports = exportsOf({file});
        check(findExportMatching(exports, Symbols::standardHashCallPrefix)
              != nullptr);
    }
};

auto tGyChangesNothing = test("ExportList/listsAGyObjectAsTheDefaultOne") = []
{
    auto plain = exportsOf({EXPORT_LIST_CLASSIC});
    auto gy = exportsOf({EXPORT_LIST_GY});

    for (const auto& entry: plain)
        check(findExport(gy, entry.name) != nullptr, entry.name);

    check(findExport(gy, Symbols::plainFunction) != nullptr);
    check(findExport(gy, Symbols::writableGlobal) != nullptr);
};

auto tBaselineExclusions = test("ExportList/excludesWhatCreateDefExcludes") = []
{
    for (auto filter: {Filter::HotReload, Filter::Unfiltered})
    {
        auto exports = exportsOf({EXPORT_LIST_CLASSIC}, filter);
        check(!anyExportStartsWith(exports, Symbols::deletingDestructorPrefix));
        check(!anyExportStartsWith(exports, Symbols::stringLiteralPrefix));
        check(!anyExportStartsWith(exports, "__real@"));
    }

    check(isExcludedName("??_GShape@@UEAAPEAXI@Z"));
    check(isExcludedName("??_EShape@@UEAAPEAXI@Z"));
    check(isExcludedName("name.with.dots"));
    check(isExcludedName("__t2m"));
    check(isExcludedName("?f@@$$FYAXXZ"));
    check(isExcludedName("=="));
    check(!isExcludedName(Symbols::plainFunction));
};

auto tVftableIsDataUnfiltered = test("ExportList/tagsAVftableAsDataUnfiltered") = []
{
    auto unfiltered = exportsOf({EXPORT_LIST_CLASSIC}, Filter::Unfiltered);
    auto vftable = findExport(unfiltered, Symbols::vftable);
    check(vftable != nullptr && vftable->data);
};

auto tStandardNamespace = test("ExportList/readsTheOutermostNamespace") = []
{
    check(isInStandardNamespace(
        "?push_back@?$vector@HV?$allocator@H@std@@@std@@QEAAXAEBH@Z"));
    check(isInStandardNamespace("??$move@AEAH@std@@YA$$QEAHAEAH@Z"));
    check(!isInStandardNamespace(
        "?size@Widget@eacp@@QEBA_KAEBV?$basic_string@DU?$char_traits@D@std@@V?$"
        "allocator@D@2@@std@@@Z"));
    check(!isInStandardNamespace(
        "??$twice@V?$vector@HV?$allocator@H@std@@@std@@@ExportListFixture@@YA?AV?"
        "$vector@HV?$allocator@H@std@@@std@@V12@@Z"));
    check(!isInStandardNamespace("plain_c_function"));
};

auto tRefusesLinkTimeCode = test("ExportList/refusesALinkTimeCodeObject") = []
{
    auto message = errorFrom([] { exportsOf({EXPORT_LIST_LINK_TIME_CODE}); });
    check(mentionsAll(message, {"LinkTimeCode", "Link-time optimisation"}), message);
};

auto tRefusesLinkTimeCodeMember =
    test("ExportList/refusesALinkTimeCodeArchiveMember") = []
{
    auto message =
        errorFrom([] { exportsOf({EXPORT_LIST_LINK_TIME_CODE_ARCHIVE}); });
    check(mentionsAll(message,
                      {"ExportListFixtureLinkTimeCodeArchive",
                       "(",
                       "LinkTimeCode",
                       "Link-time optimisation"}),
          message);
};

auto tRefusesBitcodeBytes = test("ExportList/refusesLlvmBitcode") = []
{
    for (auto magic: {std::string {"BC\xC0\xDE\x35\x14\x00\x00", 8},
                      std::string {"\xDE\xC0\x17\x0B\x00\x00\x00\x00", 8}})
    {
        check(detectObjectFormat(bytesOf(magic)) == ObjectFormat::Bitcode);

        auto message = errorFrom(
            [&]
            {
                auto collector = Collector {};
                collector.addBytes(bytesOf(magic), FilePath {"thin-lto.obj"});
            });
        check(mentionsAll(message, {"thin-lto.obj", "LLVM bitcode", "/GL"}),
              message);
    }
};

auto tRefusesMsvcLinkTimeCodeBytes = test("ExportList/refusesMsvcGlHeader") = []
{
    auto header = std::string(56, '\0');
    header[2] = '\xFF';
    header[3] = '\xFF';
    header[4] = '\x01';
    header[6] = '\x64';
    header[7] = '\x86';

    check(detectObjectFormat(bytesOf(header)) == ObjectFormat::LinkTimeCode);

    auto message = errorFrom(
        [&]
        {
            auto collector = Collector {};
            collector.addBytes(bytesOf(header), FilePath {"whole-program.obj"});
        });
    check(mentionsAll(message, {"whole-program.obj", "/GL"}), message);
};

auto tRefusesCorruptInput = test("ExportList/refusesCorruptAndUnknownInput") = []
{
    auto bytes = fileBytes(EXPORT_LIST_CLASSIC);
    auto truncated = bytes.substr(0, 64);
    auto truncatedMessage = errorFrom(
        [&]
        {
            auto collector = Collector {};
            collector.addBytes(bytesOf(truncated), FilePath {"cut-short.obj"});
        });
    check(mentionsAll(truncatedMessage, {"cut-short.obj"}), truncatedMessage);

    auto text = std::string {"not an object at all"};
    auto unknownMessage = errorFrom(
        [&]
        {
            auto collector = Collector {};
            collector.addBytes(bytesOf(text), FilePath {"notes.txt"});
        });
    check(mentionsAll(unknownMessage, {"notes.txt", "neither"}), unknownMessage);

    auto archive = fileBytes(EXPORT_LIST_ARCHIVE).substr(0, 100);
    auto archiveMessage = errorFrom(
        [&]
        {
            auto collector = Collector {};
            collector.addBytes(bytesOf(archive), FilePath {"cut-short.lib"});
        });
    check(mentionsAll(archiveMessage, {"cut-short.lib"}), archiveMessage);

    auto missingMessage =
        errorFrom([] { Collector {}.addFile(FilePath {"no-such-object.obj"}); });
    check(mentionsAll(missingMessage, {"no-such-object.obj"}), missingMessage);
};

auto tRefusesMixedMachines = test("ExportList/refusesInputsForTwoMachines") = []
{
    auto arm64 = emptyObject(0xAA64);
    auto x64 = emptyObject(0x8664);
    auto collector = Collector {};
    collector.addBytes(bytesOf(arm64), FilePath {"arm64.obj"});
    check(collector.machine() == Machine::Arm64);

    auto message =
        errorFrom([&] { collector.addBytes(bytesOf(x64), FilePath {"x64.obj"}); });
    check(mentionsAll(message, {"x64.obj", "X64", "ARM64"}), message);

    auto i386 = emptyObject(0x014C);
    auto i386Message = errorFrom(
        [&] { Collector {}.addBytes(bytesOf(i386), FilePath {"x86.obj"}); });
    check(mentionsAll(i386Message, {"x86.obj", "0x014C"}), i386Message);
};

void putU16(std::string& bytes, std::size_t at, std::uint16_t value)
{
    bytes[at] = static_cast<char>(value & 0xFF);
    bytes[at + 1] = static_cast<char>(value >> 8);
}

void putU32(std::string& bytes, std::size_t at, std::uint32_t value)
{
    putU16(bytes, at, static_cast<std::uint16_t>(value & 0xFFFF));
    putU16(bytes, at + 2, static_cast<std::uint16_t>(value >> 16));
}

// A classic object with `sections` code sections and one external function,
// "highSect", defined in the last of them, and one absolute external.
std::string objectWithManySections(std::uint16_t sections)
{
    constexpr auto headerSize = std::size_t {20};
    constexpr auto sectionSize = std::size_t {40};
    constexpr auto symbolSize = std::size_t {18};

    auto symbolTable = headerSize + sections * sectionSize;
    auto bytes = std::string(symbolTable + 2 * symbolSize + 4, '\0');
    putU16(bytes, 0, 0x8664);
    putU16(bytes, 2, sections);
    putU32(bytes, 8, static_cast<std::uint32_t>(symbolTable));
    putU32(bytes, 12, 2);

    for (auto section = std::size_t {0}; section < sections; ++section)
        putU32(bytes, headerSize + section * sectionSize + 36, 0x60000020);

    auto addSymbol =
        [&](std::size_t index, std::string_view name, std::uint16_t number)
    {
        auto at = symbolTable + index * symbolSize;
        bytes.replace(at, name.size(), name);
        putU16(bytes, at + 12, number);
        putU16(bytes, at + 14, 0x20);
        bytes[at + 16] = 2;
    };

    addSymbol(0, "highSect", sections);
    addSymbol(1, "absolute", 0xFFFF);
    putU32(bytes, symbolTable + 2 * symbolSize, 4);
    return bytes;
}

auto tHighSectionNumbers =
    test("ExportList/listsSymbolsInClassicSectionsAbove32767") = []
{
    for (auto sections: {std::uint16_t {3}, std::uint16_t {0x8001}})
    {
        auto object = objectWithManySections(sections);
        auto collector = Collector {};
        collector.addBytes(bytesOf(object), FilePath {"many-sections.obj"});
        auto exports = collector.exports(Filter::HotReload);

        check(findExport(exports, "highSect") != nullptr, std::to_string(sections));
        check(findExport(exports, "absolute") == nullptr);
    }
};

auto tNamesWhatItRefuses = test("ExportList/namesWhatARefusedInputIs") = []
{
    auto refusal = [](const std::string& bytes)
    {
        return errorFrom(
            [&] { Collector {}.addBytes(bytesOf(bytes), FilePath {"input.obj"}); });
    };

    auto arm64x = refusal(emptyObject(0xA64E));
    check(mentionsAll(arm64x, {"input.obj", "0xA64E", "ARM64X"}), arm64x);

    auto importMember = std::string(20, '\0');
    putU16(importMember, 2, 0xFFFF);
    putU16(importMember, 6, 0x014C);
    auto import = refusal(importMember);
    check(mentionsAll(import, {"input.obj", "import member", "x86"}), import);

    auto resource = std::string(32, '\0');
    putU32(resource, 4, 0x20);
    putU16(resource, 8, 0xFFFF);
    putU16(resource, 12, 0xFFFF);
    auto res = refusal(resource);
    check(mentionsAll(res, {"input.obj", ".res"}), res);
};

auto tDefinitionFile = test("ExportList/writesNameAndSortedExports") = []
{
    auto exports = Vector<Export> {{"?b@@3HA", true}, {"?a@@YAXXZ", false}};
    auto text = makeDefinitionFile("My App.exe", exports);
    check(text == "NAME \"My App.exe\"\nEXPORTS\n    ?b@@3HA DATA\n    ?a@@YAXXZ\n");

    auto sorted = exportsOf({EXPORT_LIST_CLASSIC});

    for (auto index = 1; index < sorted.size(); ++index)
        check(sorted[index - 1].name < sorted[index].name);

    check(makeDefinitionFile("App.exe", sorted)
              .starts_with("NAME App.exe\nEXPORTS\n"));
};

auto tSameInputsSameList = test("ExportList/givesTheSameListForTheSameInputs") = []
{
    auto first = exportsOf({EXPORT_LIST_ARCHIVE, EXPORT_LIST_CLASSIC});
    auto second = exportsOf({EXPORT_LIST_CLASSIC, EXPORT_LIST_ARCHIVE});
    check(makeDefinitionFile("App.exe", first)
          == makeDefinitionFile("App.exe", second));
};

auto tUnchangedOutput = test("ExportList/leavesAnUnchangedListAlone") = []
{
    auto file = TemporaryFile {".def"};
    auto text = makeDefinitionFile("App.exe", exportsOf({EXPORT_LIST_CLASSIC}));

    check(writeIfChanged(file.path(), text));
    auto written = std::filesystem::last_write_time(toStdPath(file.path()));

    std::filesystem::last_write_time(toStdPath(file.path()),
                                     written - std::chrono::hours {1});
    auto backdated = std::filesystem::last_write_time(toStdPath(file.path()));

    check(!writeIfChanged(file.path(), text));
    check(std::filesystem::last_write_time(toStdPath(file.path())) == backdated);
    check(Files::readFile(file.path()) == text);

    check(writeIfChanged(file.path(), text + "    ?extra@@YAXXZ\n"));
    check(std::filesystem::last_write_time(toStdPath(file.path())) != backdated);
};

auto tLimits = test("ExportList/warnsNearAndFailsAboveTheLimit") = []
{
    check(maximumExports == 65535);
    check(checkLimit(12000) == LimitStatus::WithinLimit);
    check(checkLimit(ExportLimits {}.warnAt - 1) == LimitStatus::WithinLimit);
    check(checkLimit(ExportLimits {}.warnAt) == LimitStatus::NearLimit);
    check(checkLimit(65535) == LimitStatus::NearLimit);
    check(checkLimit(65536) == LimitStatus::OverLimit);

    auto small = ExportLimits {2, 3};
    check(checkLimit(1, small) == LimitStatus::WithinLimit);
    check(checkLimit(2, small) == LimitStatus::NearLimit);
    check(checkLimit(3, small) == LimitStatus::NearLimit);
    check(checkLimit(4, small) == LimitStatus::OverLimit);
};
} // namespace ExportListLibraryTests
