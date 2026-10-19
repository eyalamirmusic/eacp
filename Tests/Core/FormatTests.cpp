#include "Common.h"

#include <eacp/Core/Utils/Format.h>
#include <eacp/Core/Utils/Logging.h>

#include <cstdlib>
#include <string>
#include <string_view>

using namespace nano;

namespace FormatTests
{
// Qualified, not `using eacp::Strings::format`: an unqualified call with a
// std::string argument also finds std::format by argument-dependent lookup.
namespace Strings = eacp::Strings;

enum class Fruit
{
    Apple = 3,
    Pear = 7
};

struct Point2
{
    int x = 0;
    int y = 0;
};

std::string toString(const Point2& point)
{
    return "(" + std::to_string(point.x) + ", " + std::to_string(point.y) + ")";
}

auto tNoArgs = test("Format/no arguments") = []
{
    check(Strings::format("plain text") == "plain text");
    check(Strings::format("").empty());
    check(Strings::format("{{escaped}}") == "{escaped}");
};

auto tIntegers = test("Format/integers, bool and char") = []
{
    check(Strings::format("{}", 42) == "42");
    check(Strings::format("{}", -7) == "-7");
    check(Strings::format("{}", 42u) == "42");
    check(Strings::format("{}", 18446744073709551615ull) == "18446744073709551615");
    check(Strings::format("{}", -9223372036854775807ll - 1)
          == "-9223372036854775808");
    check(Strings::format("{}", static_cast<short>(-3)) == "-3");
    check(Strings::format("{}", static_cast<unsigned char>(200)) == "200");
    check(Strings::format("{} {}", true, false) == "true false");
    check(Strings::format("{}", 'x') == "x");
    check(Strings::format("{1}-{0}", 1, 2) == "2-1");
};

auto tFloatShortest =
    test("Format/float and double print their own shortest form") = []
{
    check(Strings::format("{}", 0.1f) == "0.1");
    check(Strings::format("{}", 0.1) == "0.1");
    check(Strings::format("{}", 1.0f / 3.0f) == "0.33333334");
    check(Strings::format("{}", 1.0 / 3.0) == "0.3333333333333333");
    check(Strings::format("{}", 1.0f) == "1");
    check(Strings::format("{}", 2.5) == "2.5");
};

auto tSpecs = test("Format/width, precision, fill and hex") = []
{
    check(Strings::format("{:08X}", 0xBEEFu) == "0000BEEF");
    check(Strings::format("0x{:08x}", static_cast<unsigned long>(0x887A0005ul))
          == "0x887a0005");
    check(Strings::format("{:+.1f}", 2.26) == "+2.3");
    check(Strings::format("{:+.1f}", -0.04) == "-0.0");
    check(Strings::format("{:>6}", "ab") == "    ab");
    check(Strings::format("{:*<5}", 7) == "7****");
    check(Strings::format("{:^7}", "mid") == "  mid  ");
    check(Strings::format("{:.2f} GB", 1.5) == "1.50 GB");
    check(Strings::format("{:g}", 1000000.0) == "1e+06");
    check(Strings::format("{:.{}g}", 3.14159f, 3) == "3.14");
    check(Strings::format("\\u{:04x}", 0x1f) == "\\u001f");
};

auto tStrings = test("Format/string_view, std::string and const char*") = []
{
    const auto owned = std::string {"owned"};
    const auto view = std::string_view {"view"};
    const char* pointer = "pointer";

    check(Strings::format("{} {} {} {}", owned, view, pointer, "literal")
          == "owned view pointer literal");
    check(Strings::format("[{:<6}]", owned) == "[owned ]");
};

auto tToString = test("Format/other types go through Strings::toString") = []
{
    check(Strings::format("{} and {}", Fruit::Apple, Fruit::Pear) == "3 and 7");
    check(Strings::format("at {}", Point2 {1, 2}) == "at (1, 2)");
    check(Strings::format("[{:>8}]", Point2 {3, 4}) == "[  (3, 4)]");
};

auto tMalformed = test("Format/a malformed pattern returns an error string") = []
{
    auto text = Strings::format("{} {", 1);
    check(text.find("{} {") != std::string::npos);
    check(text.find("format error") != std::string::npos);

    text = Strings::format("{} {}", 1);
    check(text.find("format error") != std::string::npos);

    text = Strings::format("{:d}", "not a number");
    check(text.find("format error") != std::string::npos);
};

auto tShortestRoundTrip =
    test("Format/shortestRoundTrip keeps %g notation and round-trips") = []
{
    check(Strings::shortestRoundTrip(0.1f) == "0.1");
    check(Strings::shortestRoundTrip(1.0f) == "1");
    check(Strings::shortestRoundTrip(-2.5f) == "-2.5");
    check(Strings::shortestRoundTrip(16777215.0f) == "16777215");
    check(Strings::shortestRoundTrip(1e-7f) == "1e-07");
    check(Strings::shortestRoundTrip(1.0f / 3.0f) == "0.33333334");

    for (auto value: {1.0f / 3.0f, 0.1f, 3.14159265f, 1e-7f, 123456.789f, 1e30f})
    {
        const auto text = Strings::shortestRoundTrip(value);
        check(std::strtof(text.c_str(), nullptr) == value);
    }
};

auto tLogf = test("Format/LOGF logs a formatted line") = []
{
    eacp::LOGF("Format test: {} is 0x{:04X}", "value", 0xABu);
    check(true);
};
} // namespace FormatTests
