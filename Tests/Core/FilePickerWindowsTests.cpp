#include <eacp/Core/App/App-Windows-FilePicker.h>
#include <eacp/Core/App/App.h>

#include <NanoTest/NanoTest.h>

#include <optional>
#include <string>

using namespace nano;
using eacp::Apps::FilePickerOptions;
using eacp::Apps::detail::buildFilterPattern;
using eacp::Apps::detail::chooseWithDialog;
using eacp::Apps::detail::shellResultToPath;

auto tFilterEmpty = test("FilePicker/Windows/emptyExtensionsGiveEmptyPattern") = []
{ check(buildFilterPattern({}).empty()); };

auto tFilterSingle = test("FilePicker/Windows/singleExtension") = []
{ check(buildFilterPattern({"wav"}) == std::wstring(L"*.wav")); };

auto tFilterMany = test("FilePicker/Windows/joinsExtensionsWithSemicolons") = []
{
    check(buildFilterPattern({"wav", "mp3", "flac"})
          == std::wstring(L"*.wav;*.mp3;*.flac"));
};

auto tCancelNull = test("FilePicker/Windows/nullSelectionIsCancel") = []
{ check(!shellResultToPath(nullptr).has_value()); };

auto tCancelEmpty = test("FilePicker/Windows/emptySelectionIsCancel") = []
{ check(!shellResultToPath(L"").has_value()); };

auto tAsciiPath = test("FilePicker/Windows/asciiPathRoundTrips") = []
{
    const auto path = shellResultToPath(L"C:\\Users\\me\\Music");
    check(path.has_value());
    check(*path == std::string("C:\\Users\\me\\Music"));
};

auto tUnicodePath = test("FilePicker/Windows/unicodePathEncodesToUtf8") = []
{
    // Escapes not glyphs, so the test is independent of source encoding:
    // U+00FC -> UTF-8 0xC3 0xBC.
    const auto path = shellResultToPath(L"C:\\M\u00fcsic");
    check(path.has_value());
    check(*path
          == std::string("C:\\M\xC3\xBC"
                         "sic"));
};

auto tHonoursSelection = test("FilePicker/Windows/returnsThePickedDirectory") = []
{
    const auto picked = chooseWithDialog(
        true,
        {},
        [](bool, const FilePickerOptions&) -> std::optional<std::wstring>
        { return L"C:\\Users\\me\\Samples"; });

    check(picked.has_value());
    check(*picked == std::string("C:\\Users\\me\\Samples"));
};

auto tCancelYieldsNullopt = test("FilePicker/Windows/cancelReturnsNullopt") = []
{
    const auto picked = chooseWithDialog(
        true,
        {},
        [](bool, const FilePickerOptions&) -> std::optional<std::wstring>
        { return std::nullopt; });

    check(!picked.has_value());
};

auto tForwardsPickFoldersFlag =
    test("FilePicker/Windows/forwardsFolderModeToDialog") = []
{
    auto sawPickFolders = std::optional<bool> {};
    chooseWithDialog(true,
                     {},
                     [&](bool pickFolders,
                         const FilePickerOptions&) -> std::optional<std::wstring>
                     {
                         sawPickFolders = pickFolders;
                         return std::nullopt;
                     });

    check(sawPickFolders.has_value());
    check(*sawPickFolders == true);
};
