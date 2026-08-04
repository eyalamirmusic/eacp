#include "Common.h"

#include <eacp/Core/Utils/Strings.h>

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
