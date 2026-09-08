#include "Common.h"

#include <sstream>
#include <string>

// The Unicode Bidirectional Algorithm over a curated subset of the official
// BidiCharacterTest.txt of Unicode 16.0, one group of cases per rule group,
// plus the byte runs and the mirroring the Linux rasterizer reads off it.
// Portable logic over portable tables, so it runs on all three platforms even
// though only the Linux backend calls it: CoreText and DirectWrite run their
// own bidi inside CTLine and IDWriteTextLayout.

using namespace nano;
using namespace eacp;
using namespace eacp::Text;

namespace
{
// One line of BidiCharacterTest.txt in that file's own fields: the
// codepoints, the paragraph direction (0 left-to-right, 1 right-to-left,
// 2 the algorithm deciding for itself), the paragraph level that came out,
// the resolved level of every codepoint with "x" where X9 removed it, and the
// indices of the rest in visual order.
struct ConformanceCase
{
    const char* codepoints;
    int direction;
    int paragraphLevel;
    const char* levels;
    const char* order;
};

constexpr ConformanceCase conformanceCases[] = {
    // X5c, the first-strong isolate
    {"202D 05D0 202B 05D1 202C 2068 05D2 2069 202B 05D3 202C 05D4 202C",
     2,
     1,
     "x 2 x 3 x 2 3 2 x 3 x 2 x",
     "1 3 5 6 7 9 11"},
    {"202D 0661 202B 0662 202C 2068 0663 2069 202B 0664 202C 0665 202C",
     0,
     0,
     "x 2 x 4 x 2 6 2 x 4 x 2 x",
     "1 3 5 6 7 9 11"},
    {"0061 0028 0062 005B 0063 2068 05D0 2069 0064 005D 0065 0029 0066",
     0,
     0,
     "0 0 0 0 0 0 1 0 0 0 0 0 0",
     "0 1 2 3 4 5 6 7 8 9 10 11 12"},
    {"0061 0028 0062 005B 0063 2068 05D0 2069 0064 005D 0065 0029 0066",
     1,
     1,
     "2 2 2 2 2 2 3 2 2 2 2 2 2",
     "0 1 2 3 4 5 6 7 8 9 10 11 12"},

    // X5a and X6a, a left-to-right isolate
    {"05D0 2066 202A 2069 05D1", 0, 0, "1 1 x 1 1", "4 3 1 0"},
    {"05D0 2066 202B 2069 05D1", 0, 0, "1 1 x 1 1", "4 3 1 0"},
    {"05D0 2066 202C 2069 05D1", 0, 0, "1 1 x 1 1", "4 3 1 0"},

    // X5b and X6a, a right-to-left isolate
    {"0061 2067 202A 2069 0062", 1, 1, "2 2 x 2 2", "0 1 3 4"},
    {"0061 2067 202C 202E 2069 0062", 1, 1, "2 2 x x 2 2", "0 1 4 5"},
    {"0061 0028 0062 2067 05D0 0066 2069 05D4 0029 05D5",
     0,
     0,
     "0 0 0 0 1 2 0 1 0 1",
     "0 1 2 3 5 4 6 7 8 9"},

    // X2 to X5 and X7, embeddings and their terminator
    {"202A 05D0 0028 05D1 202C 202D 0029", 2, 1, "x 3 3 3 x x 2", "3 2 1 6"},
    {"202A 05D0 0028 05D1 202C 202D 0029 202C", 2, 1, "x 3 3 3 x x 2 x", "3 2 1 6"},
    {"202B 0061 0028 0062 202C 202E 0029", 2, 0, "x 2 2 2 x x 1", "6 1 2 3"},
    {"202B 0061 0028 0062 202C 202E 0029 202C", 2, 0, "x 2 2 2 x x 1 x", "6 1 2 3"},
    {"202A 202E 0061 202C 0028 05D0 202C 202D 0029 202C",
     2,
     0,
     "x x 3 x 3 3 x x 2 x",
     "5 4 2 8"},

    // X6, an override rewriting the characters under it
    {"202B 202D 05D0 202C 0028 0061 202C 202E 0029 202C",
     2,
     1,
     "x x 4 x 4 4 x x 3 x",
     "8 2 4 5"},
    {"202D 0028 202C 202A 05D0 0029 05D1", 2, 1, "x 2 x x 3 3 3", "1 6 5 4"},
    {"202D 0028 202C 202A 05D0 0029 05D1 202C", 2, 1, "x 2 x x 3 3 3 x", "1 6 5 4"},
    {"202E 0028 202C 202B 0061 0029 0062", 2, 0, "x 1 x x 2 2 2", "4 5 6 1"},
    {"202E 0028 202C 202B 0061 0029 0062 202C", 2, 0, "x 1 x x 2 2 2 x", "4 5 6 1"},

    // W1, a mark taking the type of what it sits on
    {"0061 0028 0062 0029 0331", 1, 1, "2 2 2 2 2", "0 1 2 3 4"},
    {"0061 0028 0332 0062 0029 0333", 1, 1, "2 2 2 2 2 2", "0 1 2 3 4 5"},
    {"05D0 0028 05D1 0029 0331", 0, 0, "1 1 1 1 1", "4 3 2 1 0"},
    {"05D0 0028 0332 05D1 0029 0333", 0, 0, "1 1 1 1 1 1", "5 4 3 2 1 0"},
    {"0661 0028 0662 0029 0331", 0, 0, "2 1 2 1 1", "4 3 2 1 0"},
    {"0661 0028 0332 0662 0029 0333", 0, 0, "2 1 1 2 1 1", "5 4 3 2 1 0"},

    // W2 and W7, a number after an Arabic letter or a Latin one
    {"0661 002D 0031", 0, 0, "2 0 0", "0 1 2"},
    {"0031 0661 0028 0627 0029", 0, 0, "0 2 1 1 1", "0 4 3 2 1"},

    // W4 to W6, separators and terminators around numbers
    {"062A 0031 002F 0032", 2, 1, "1 2 2 2", "1 2 3 0"},
    {"062A 0031 002F 0032", 0, 0, "1 2 2 2", "1 2 3 0"},
    {"062A 0031 002F 0032", 1, 1, "1 2 2 2", "1 2 3 0"},
    {"0028 05D0 0029 0020 0031 002E 0032", 0, 0, "0 1 0 0 2 2 2", "0 1 2 3 4 5 6"},
    {"0028 0627 0029 0020 0031 002E 0032", 0, 0, "0 1 0 0 2 2 2", "0 1 2 3 4 5 6"},
    {"002B 0661 0028 0662 0029", 2, 0, "0 2 1 2 1", "0 4 3 2 1"},

    // N0, bracket pairs
    {"0627 0628 062C 0020 0062 006F 006F 006B 0028 0073 0029",
     0,
     0,
     "1 1 1 0 0 0 0 0 0 0 0",
     "2 1 0 3 4 5 6 7 8 9 10"},
    {"0627 0628 062C 0020 0062 006F 006F 006B 0028 0073 0029",
     1,
     1,
     "1 1 1 1 2 2 2 2 2 2 2",
     "4 5 6 7 8 9 10 3 2 1 0"},
    {"202A 202E 0061 202C 0028 005B 05D0 202C 202D 005D 0029 202C",
     2,
     0,
     "x x 3 x 3 3 3 x x 2 2 x",
     "6 5 4 2 9 10"},
    {"202B 202D 05D0 202C 0028 005B 0061 202C 202E 005D 0029 202C",
     2,
     1,
     "x x 4 x 4 4 4 x x 3 3 x",
     "10 9 2 4 5 6"},
    {"202D 202E 0061 202C 0028 202C 202A 05D0 0029 05D1",
     2,
     0,
     "x x 3 x 2 x x 3 3 3",
     "2 4 9 8 7"},
    {"202E 202D 05D0 202C 0028 202C 202B 0061 0029 0062",
     2,
     1,
     "x x 4 x 3 x x 4 4 4",
     "7 8 9 4 2"},
    {"202D 202E 0061 202C 0028 005B 202C 202A 05D0 005D 0029 05D1",
     2,
     0,
     "x x 3 x 2 2 x x 3 3 3 3",
     "2 4 5 11 10 9 8"},
    {"202E 202D 05D0 202C 0028 005B 202C 202B 0061 005D 0029 0062",
     2,
     1,
     "x x 4 x 3 3 x x 4 4 4 4",
     "8 9 10 11 5 4 2"},
    {"0041 200F 005B 05D0 005D 200D 20D6", 0, 0, "0 1 1 1 1 x 1", "0 6 4 3 2 1"},

    // N0, brackets matched under canonical equivalence
    {"0061 0020 2329 0062 002E 0031 232A", 1, 1, "2 2 2 2 2 2 2", "0 1 2 3 4 5 6"},
    {"0061 0020 3008 0062 002E 0031 3009", 1, 1, "2 2 2 2 2 2 2", "0 1 2 3 4 5 6"},
    {"0061 0020 2329 0062 002E 0031 3009", 1, 1, "2 2 2 2 2 2 2", "0 1 2 3 4 5 6"},
    {"0061 0020 3008 0062 002E 0031 232A", 1, 1, "2 2 2 2 2 2 2", "0 1 2 3 4 5 6"},

    // L1, a segment separator and the whitespace before it
    {"0661 0009 0028 0662 0029", 2, 0, "2 0 1 2 1", "0 1 4 3 2"},

    // X9, characters the algorithm removes
    {"05D0 2066 2060 2069 05D1", 0, 0, "1 1 x 1 1", "4 3 1 0"},
    {"00AD 0028 2069 0661 0025 0029 0662", 2, 0, "x 0 0 2 0 0 2", "1 2 3 4 5 6"},

    // P2 and P3, the base direction the text itself decides
    {"0025 0661 0028 0662 0029", 2, 0, "0 2 1 2 1", "0 4 3 2 1"},
    {"0661 0020 0028 0662 0029", 2, 0, "2 1 1 2 1", "4 3 2 1 0"},
    {"202A 0661 0028 05D0 0029", 2, 1, "x 4 3 3 3", "4 3 2 1"},
    {"202A 0661 0028 0662 0029", 2, 0, "x 4 3 4 3", "4 3 2 1"},
    {"202C 0661 0028 0662 0029", 2, 0, "x 2 1 2 1", "4 3 2 1"},
    {"0661 2069 0028 0662 0029", 2, 0, "2 1 1 2 1", "4 3 2 1 0"},

    // L2, plain mixed-direction text
    {"0061 0028 0062 202B 202C 0029 0020 05D0",
     1,
     1,
     "2 2 2 x x 2 1 1",
     "7 6 0 1 2 5"},
    {"0061 0028 05D0 0029", 0, 0, "0 0 1 0", "0 1 2 3"},
    {"0061 0028 05D0 0029", 1, 1, "2 1 1 1", "3 2 1 0"},
    {"0061 0028 0029 05D0", 0, 0, "0 0 0 1", "0 1 2 3"},
    {"0061 0028 0029 05D0", 1, 1, "2 1 1 1", "3 2 1 0"},
    {"0028 0061 05D0 0029", 0, 0, "0 0 1 0", "0 1 2 3"},

    // N1 and N2, neutrals between Arabic and digits
    {"0627 202A 202C 0020 0031 002D 0032", 0, 0, "1 x x 1 2 1 2", "6 5 4 3 0"},
    {"0627 202A 002A 202C 0020 0031 002D 0032",
     0,
     0,
     "1 x 2 x 0 0 0 0",
     "2 0 4 5 6 7"},
    {"0627 202B 202C 0020 0031 002D 0032", 0, 0, "1 x x 1 2 1 2", "6 5 4 3 0"},
    {"0627 202B 002A 202C 0020 0031 002D 0032",
     0,
     0,
     "1 x 1 x 1 2 2 2",
     "5 6 7 4 2 0"},
};

Vector<std::string> fieldsOf(const char* text)
{
    auto stream = std::istringstream {text};
    auto result = Vector<std::string> {};
    auto word = std::string {};

    while (stream >> word)
        result.add(word);

    return result;
}

Vector<char32_t> codepointsOf(const char* text)
{
    auto result = Vector<char32_t> {};

    for (const auto& word: fieldsOf(text))
        result.add((char32_t) std::stoul(word, nullptr, 16));

    return result;
}

BidiBaseDirection baseOf(int direction)
{
    if (direction == 0)
        return BidiBaseDirection::LeftToRight;

    return direction == 1 ? BidiBaseDirection::RightToLeft : BidiBaseDirection::Auto;
}

std::string utf8Of(std::initializer_list<char32_t> codepoints)
{
    auto text = std::string {};

    for (const auto codepoint: codepoints)
    {
        char encoded[4] = {};

        text.append(encoded, (std::size_t) encodeUtf8(codepoint, encoded));
    }

    return text;
}
} // namespace

auto tBidiConformance = test("Text/bidiConformance") = []
{
    for (const auto& testCase: conformanceCases)
    {
        const auto text = codepointsOf(testCase.codepoints);
        const auto resolved = bidiLevels(text, baseOf(testCase.direction));

        check(resolved.paragraphLevel == testCase.paragraphLevel,
              testCase.codepoints);

        const auto expected = fieldsOf(testCase.levels);
        check(resolved.levels.size() == expected.size(), testCase.codepoints);

        for (auto index = 0; index < expected.size(); ++index)
        {
            const auto level = (int) resolved.levels[index];

            if (expected[index] == "x")
                check(level == bidiRemovedLevel, testCase.codepoints);
            else
                check(level == std::stoi(expected[index]), testCase.codepoints);
        }

        const auto order = bidiVisualOrder(resolved);
        const auto expectedOrder = fieldsOf(testCase.order);

        check(order.size() == expectedOrder.size(), testCase.codepoints);

        for (auto index = 0; index < expectedOrder.size(); ++index)
            check(order[index] == std::stoi(expectedOrder[index]),
                  testCase.codepoints);
    }
};

auto tBidiClassTable = test("Text/bidiClassTable") = []
{
    check(bidiClassOf(U'a') == BidiClass::L);
    check(bidiClassOf(U'0') == BidiClass::EN);
    check(bidiClassOf(U' ') == BidiClass::WS);
    check(bidiClassOf(U'(') == BidiClass::ON);

    // Hebrew is R, Arabic letters are AL and Arabic-Indic digits are AN -
    // the distinction W2 turns on.
    check(bidiClassOf(0x05D0) == BidiClass::R);
    check(bidiClassOf(0x0627) == BidiClass::AL);
    check(bidiClassOf(0x0661) == BidiClass::AN);

    check(bidiClassOf(0x2066) == BidiClass::LRI);
    check(bidiClassOf(0x2067) == BidiClass::RLI);
    check(bidiClassOf(0x2068) == BidiClass::FSI);
    check(bidiClassOf(0x2069) == BidiClass::PDI);
    check(bidiClassOf(0x202C) == BidiClass::PDF);
    check(bidiClassOf(0x00AD) == BidiClass::BN);
    check(bidiClassOf(0x0301) == BidiClass::NSM);

    // An unassigned codepoint in a right-to-left block is R, not L: the
    // @missing default the derived file carries and a plain UnicodeData walk
    // would miss.
    check(bidiClassOf(0x05EB) == BidiClass::R);
    check(bidiClassOf(0x08B5) == BidiClass::AL);
};

auto tBidiMirroring = test("Text/bidiMirroring") = []
{
    check(bidiMirroredGlyph(U'(') == U')');
    check(bidiMirroredGlyph(U')') == U'(');
    check(bidiMirroredGlyph(U'<') == U'>');
    check(bidiMirroredGlyph(U'[') == U']');

    // A character with no mirror is its own.
    check(bidiMirroredGlyph(U'a') == U'a');
    check(bidiMirroredGlyph(0x05D0) == 0x05D0);

    // L4 only mirrors inside a right-to-left run.
    check(bidiMirroredAt(U'(', 0) == U'(');
    check(bidiMirroredAt(U'(', 1) == U')');
    check(bidiMirroredAt(U'(', 2) == U'(');
};

auto tBidiRunsCoverEveryByte = test("Text/bidiRunsCoverEveryByte") = []
{
    // "abc " then two Hebrew letters: one left-to-right run and one
    // right-to-left run, the Hebrew drawn first from the right.
    const auto text = utf8Of({U'a', U'b', U'c', U' ', 0x05D0, 0x05D1});
    const auto runs = bidiRuns(text);

    check(runs.size() == 2);
    check(!runs[0].isRightToLeft());
    check(runs[0].begin == 0 && runs[0].end == 4);
    check(runs[1].isRightToLeft());
    check(runs[1].begin == 4 && runs[1].end == (int) text.size());

    // Latin only: one run, and the fast path that skips the algorithm.
    check(isLeftToRightOnly("abc 123"));
    check(!isLeftToRightOnly(text));

    const auto latin = bidiRuns("abc 123");
    check(latin.size() == 1);
    check(latin[0].begin == 0 && latin[0].end == 7 && !latin[0].isRightToLeft());

    check(bidiRuns("").empty());
};

auto tBidiRunsAreVisuallyOrdered = test("Text/bidiRunsAreVisuallyOrdered") = []
{
    // Two Hebrew letters, a space, "ab", a space, two more Hebrew letters:
    // the paragraph is right-to-left, so the Latin island comes out in the
    // middle with the Hebrew either side of it reversed.
    const auto text =
        utf8Of({0x05D0, 0x05D1, U' ', U'a', U'b', U' ', 0x05D2, 0x05D3});
    const auto runs = bidiRuns(text);

    check(runs.size() == 3);

    // Visually first is the last Hebrew pair, then the Latin, then the first.
    check(runs[0].isRightToLeft() && runs[0].begin > runs[2].begin);
    check(!runs[1].isRightToLeft());
    check(runs[2].isRightToLeft() && runs[2].begin == 0);

    // A caller can force the base direction rather than let P2 find it.
    const auto latin = utf8Of({U'a', U'b', U' ', 0x05D0, 0x05D1});

    check(bidiRuns(latin, BidiBaseDirection::LeftToRight)[0].begin == 0);
    check(bidiRuns(latin, BidiBaseDirection::RightToLeft)[0].begin != 0);
};

auto tBidiDigitsRunLeftToRight = test("Text/bidiDigitsRunLeftToRight") = []
{
    // An Arabic letter then "12": I1 and I2 put the digits at an even level
    // inside the odd run, so they are their own left-to-right run and read
    // in the order they were typed.
    const auto text = utf8Of({0x0627, 0x0628, U' ', U'1', U'2'});
    const auto runs = bidiRuns(text);

    check(runs.size() == 2);
    check(!runs[0].isRightToLeft());
    check(runs[1].isRightToLeft());

    // The digits are visually first, the Arabic after them.
    check(runs[0].begin > runs[1].begin);
};
