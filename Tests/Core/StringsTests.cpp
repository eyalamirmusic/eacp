#include "Common.h"

#include <eacp/Core/Utils/Strings.h>

#include <clocale>

using namespace nano;
using eacp::Strings::widen;

// widen() is the one UTF-8 -> wide converter in the framework, so the cases that
// used to differ between the hand-written copies are pinned here: what happens to
// malformed input, and what an astral codepoint becomes on a platform where
// wchar_t is too narrow to hold it. Codepoints are spelled as escapes so nothing
// depends on how the compiler reads this file.

auto tWidenEmpty = test("Strings/widen empty") = [] { check(widen("").empty()); };

auto tWidenAscii =
    test("Strings/widen ASCII") = [] { check(widen("hello") == L"hello"); };

auto tWidenMultiByteBmp = test("Strings/widen multi-byte BMP") = []
{
    check(widen("caf\xC3\xA9") == L"caf\u00E9"); // U+00E9, two bytes
    check(widen("\xE2\x82\xAC") == L"\u20AC"); // U+20AC, three bytes
};

auto tWidenAstral = test("Strings/widen astral codepoint") = []
{
    // U+1F600: one codepoint, but two code units wherever wchar_t is 16 bits.
    const auto wide = widen("\xF0\x9F\x98\x80");

    if constexpr (sizeof(wchar_t) == 2)
    {
        check(wide.size() == 2);
        check(wide[0] == wchar_t(0xD83D));
        check(wide[1] == wchar_t(0xDE00));
    }
    else
    {
        check(wide.size() == 1);
        check(wide[0] == wchar_t(0x1F600));
    }
};

auto tWidenInvalidLeadByte = test("Strings/widen replaces an invalid lead byte") = []
{
    check(widen("a\xFF"
                "b")
          == L"a\uFFFDb");
};

auto tWidenResynchronises =
    test("Strings/widen resumes at the next byte after bad input") = []
{
    // A three-byte lead followed by one continuation byte and then text. Each bad
    // byte costs one U+FFFD and no more, so the "ok" after the damage survives -
    // which is what discarding the whole sequence would swallow.
    check(widen("\xE2\x82"
                "ok")
          == L"\uFFFD\uFFFDok");
};

auto tWidenRejectsOverlong = test("Strings/widen replaces an overlong encoding") = []
{
    // NUL spelled with two bytes. Accepting these is how a length check gets
    // walked past, so it decodes to U+FFFD rather than to U+0000.
    check(widen("\xC0\x80") == L"\uFFFD");
};

auto tWidenRejectsEncodedSurrogate =
    test("Strings/widen replaces a surrogate half") = []
{
    // U+D800 spelled in UTF-8 (CESU-8). Not a scalar value, so not decodable.
    check(widen("\xED\xA0\x80") == L"\uFFFD");
};

auto tWidenRejectsOutOfRange =
    test("Strings/widen replaces a codepoint past U+10FFFF") = []
{ check(widen("\xF4\x90\x80\x80") == L"\uFFFD"); };

auto tWidenNeverThrows = test("Strings/widen never throws on bad input") = []
{
    // The contract every caller now relies on: no exception, no failure code.
    check(widen("\xFF\xFE\xFD") == L"\uFFFD\uFFFD\uFFFD");
};

// narrow() is the reverse leg, and the failure it has to handle is the mirror
// image: a surrogate half with no partner, which has no scalar value to encode.

using eacp::Strings::narrow;

auto tNarrowEmpty =
    test("Strings/narrow empty") = [] { check(narrow(L"").empty()); };

auto tNarrowAscii =
    test("Strings/narrow ASCII") = [] { check(narrow(L"hello") == "hello"); };

auto tNarrowMultiByteBmp = test("Strings/narrow multi-byte BMP") = []
{
    check(narrow(L"caf\u00E9") == "caf\xC3\xA9");
    check(narrow(L"\u20AC") == "\xE2\x82\xAC");
};

auto tNarrowAstral = test("Strings/narrow astral codepoint") = []
{ check(narrow(L"\U0001F600") == "\xF0\x9F\x98\x80"); };

auto tNarrowUnpairedLead = test("Strings/narrow replaces an unpaired lead") = []
{ check(narrow(std::wstring(1, wchar_t(0xD800))) == "\xEF\xBF\xBD"); };

auto tNarrowUnpairedTrail = test("Strings/narrow replaces an unpaired trail") = []
{ check(narrow(std::wstring(1, wchar_t(0xDC00))) == "\xEF\xBF\xBD"); };

auto tNarrowLeadFollowedByText =
    test("Strings/narrow keeps text after an unpaired lead") = []
{
    auto wide = std::wstring {};
    wide.push_back(wchar_t(0xD83D));
    wide.push_back(L'x');

    check(narrow(wide)
          == "\xEF\xBF\xBD"
             "x");
};

auto tRoundTrip = test("Strings/widen and narrow round-trip valid UTF-8") = []
{
    const auto original = std::string {"caf\xC3\xA9 \xE2\x82\xAC "
                                       "\xF0\x9F\x98\x80 ok"};

    check(narrow(widen(original)) == original);
};

// trim() takes a string_view now, so it absorbs the callers that used to hand it
// a substr of a view without materialising a string first.

using eacp::Strings::trim;

auto tTrim = test("Strings/trim") = []
{
    check(trim("  hello  ") == "hello");
    check(trim("\t\r\nhello\n\r\t") == "hello");
    check(trim("hello") == "hello");
    check(trim("").empty());
    check(trim("   ").empty());
};

auto tTrimAcceptsAView = test("Strings/trim accepts a string_view") = []
{
    const auto text = std::string_view {"[ padded ]"};
    check(trim(text.substr(1, 8)) == "padded");
};

// capitalize/uncapitalize touch the first character only — the whole point is
// that "Escape" must not become "ESCAPE".

auto tCapitalize = test("Strings/capitalize") = []
{
    check(eacp::Strings::capitalize("s") == "S");
    check(eacp::Strings::capitalize("escape") == "Escape");
    check(eacp::Strings::capitalize("Escape") == "Escape");
    check(eacp::Strings::capitalize("F5") == "F5");
    check(eacp::Strings::capitalize("myApiName") == "MyApiName");
    check(eacp::Strings::capitalize("+") == "+");
    check(eacp::Strings::capitalize("").empty());
};

auto tUncapitalize = test("Strings/uncapitalize") = []
{
    check(eacp::Strings::uncapitalize("Clock") == "clock");
    check(eacp::Strings::uncapitalize("clock") == "clock");
    check(eacp::Strings::uncapitalize("ABC") == "aBC");
    check(eacp::Strings::uncapitalize("").empty());
};

auto tToLower = test("Strings/toLower") = []
{
    check(eacp::Strings::toLower(".WAV") == ".wav");
    check(eacp::Strings::toLower("MiXeD") == "mixed");
    check(eacp::Strings::toLower("").empty());
};

auto tTryParseInteger = test("Strings/tryParse integers are strict") = []
{
    using eacp::Strings::tryParse;

    check(tryParse<int>("42") == 42);
    check(tryParse<int>("-7") == -7);
    check(!tryParse<int>("4x"));
    check(!tryParse<int>(" 4"));
    check(!tryParse<int>(""));
    check(!tryParse<unsigned>("-1"));
};

auto tTryParseFloating = test("Strings/tryParse reads float and double") = []
{
    using eacp::Strings::tryParse;

    check(tryParse<double>("1.5") == 1.5);
    check(tryParse<double>("-0.25") == -0.25);
    check(tryParse<double>("1e3") == 1000.0);
    check(tryParse<double>(".5") == 0.5);
    check(tryParse<float>("2.5") == 2.5f);
    check(tryParse<double>("inf").has_value());
};

auto tTryParseFloatingStrict =
    test("Strings/tryParse floating rejects what from_chars would") = []
{
    using eacp::Strings::tryParse;

    check(!tryParse<double>(""));
    check(!tryParse<double>("-"));
    check(!tryParse<double>("1.5x"));
    check(!tryParse<double>("1e"));
    check(!tryParse<double>(" 1.5"));
    check(!tryParse<double>("1.5 "));
    check(!tryParse<double>("+1.5"));
    check(!tryParse<double>("0x10"));
    check(!tryParse<double>("1,5"));
    check(!tryParse<double>("--1"));
    check(!tryParse<double>(std::string_view {"1\0"
                                              "5",
                                              3}));
    check(!tryParse<float>("1e60"));
    check(!tryParse<double>("1e400"));
};

auto tTryParseFloatingLocale =
    test("Strings/tryParse floating ignores a comma-decimal locale") = []
{
    auto previous = std::string {std::setlocale(LC_NUMERIC, nullptr)};

    // Passes trivially where no such locale is installed.
    for (auto name: {"de_DE.UTF-8", "de_DE", "German_Germany"})
    {
        if (std::setlocale(LC_NUMERIC, name) == nullptr)
            continue;

        check(eacp::Strings::tryParse<double>("1.5") == 1.5);
        check(!eacp::Strings::tryParse<double>("1,5"));
        break;
    }

    std::setlocale(LC_NUMERIC, previous.c_str());
};

auto tTryParseFloatingView = test("Strings/tryParse reads only the view") = []
{
    // The view's end is not a terminator, so this proves the copy is bounded.
    auto text = std::string_view {"2.5junk"}.substr(0, 3);
    check(eacp::Strings::tryParse<double>(text) == 2.5);
};

auto tTryParseBool = test("Strings/tryParseBool") = []
{
    using eacp::Strings::tryParseBool;

    for (auto word: {"true", "TRUE", "Yes", "on", "1"})
        check(tryParseBool(word) == true);

    for (auto word: {"false", "False", "NO", "off", "0"})
        check(tryParseBool(word) == false);

    for (auto word: {"", "maybe", "2", " true", "yes "})
        check(!tryParseBool(word).has_value());
};
