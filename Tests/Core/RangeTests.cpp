#include <eacp/Core/Utils/Range.h>
#include <NanoTest/NanoTest.h>

#include <cstdint>

using namespace nano;
using eacp::Range;

auto tDefaultRangeIsEmpty = test("Range/defaultIsEmpty") = []
{
    auto r = Range<std::uint64_t> {};
    check(r.empty());
    check(r.start == 0);
    check(r.length == 0);
    check(r.end() == 0);
};

auto tEndIsStartPlusLength = test("Range/endIsStartPlusLength") = []
{
    auto r = Range<std::uint64_t> {10, 5};
    check(!r.empty());
    check(r.end() == 15);
};

auto tZeroLengthIsEmptyRegardlessOfStart = test("Range/zeroLengthEmpty") = []
{
    auto r = Range<std::uint64_t> {100, 0};
    check(r.empty());
    check(r.end() == 100);
};

auto tContains = test("Range/contains") = []
{
    auto r = Range<int> {10, 5}; // [10, 15)
    check(!r.contains(9));
    check(r.contains(10));
    check(r.contains(14));
    check(!r.contains(15));
};

auto tEquality = test("Range/equality") = []
{
    check((Range<int> {1, 2} == Range<int> {1, 2}));
    check((Range<int> {1, 2} != Range<int> {1, 3}));
};
