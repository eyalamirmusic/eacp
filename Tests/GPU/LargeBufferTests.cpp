#include "Common.h"

#include <climits>
#include <cstdint>
#include <type_traits>

// Byte counts on the Buffer API are 64-bit, and this is the suite that says so.
//
// The two halves are checked at the level each can go wrong at. That the types
// are wide at all is a compile-time question with no run-time evidence - a count
// that narrowed back to int would still pass every small test in the tree - so
// it is pinned with static_assert. That the whole path from a count through an
// offset to the bytes on the device stays wide is a number, and the only way to
// get one is a buffer larger than an int can address: a machine that will not
// give us the allocation skips rather than fails, since what is under test is
// the arithmetic and not this machine's memory.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// 2.5 GiB: past INT_MAX by enough that the offset of the tail is too, which is
// the number that used to wrap.
constexpr std::int64_t largeBufferBytes = 2560ll * 1024 * 1024;

constexpr std::uint32_t tailMarker = 0xfeedfaceu;
constexpr std::uint32_t headMarker = 0x0badc0deu;

static_assert(
    std::is_same_v<decltype(std::declval<const Buffer&>().size()), std::int64_t>);
static_assert(std::is_same_v<decltype(BufferRange::offset), std::int64_t>);
static_assert(std::is_same_v<decltype(BufferRange::bytes), std::int64_t>);
static_assert(std::is_same_v<decltype(std::declval<StreamingBuffers&>().write(
                                 nullptr, std::int64_t {})),
                             BufferRange>);

// A range that could not be described at all before: its end is past what an
// int holds, and both fields have to be wide for the sum to be the real one.
constexpr auto tailRange = BufferRange {nullptr, largeBufferBytes - 4, 4};

static_assert(tailRange.offset > (std::int64_t) INT_MAX);
static_assert(tailRange.offset + tailRange.bytes == largeBufferBytes);
} // namespace

// The compile-time half as a case of its own, so the suite reports on the rule
// rather than only failing to build when it is broken.
auto tByteCountsAreSixtyFourBit = test("LargeBuffer/byteCountsAreSixtyFourBit") = []
{
    check(sizeof(decltype(std::declval<const Buffer&>().size())) == 8);
    check(sizeof(BufferRange::offset) == 8);
    check(sizeof(BufferRange::bytes) == 8);

    // The arithmetic a 2048-row logits buffer does, evaluated on the CPU: as
    // ints this is negative.
    constexpr std::int64_t rowBytes = 256ll * 1024;
    constexpr std::int64_t rows = 16 * 1024;

    check(rowBytes * rows > (std::int64_t) INT_MAX);
};

// The run-time half. A buffer larger than an int can address, written and read
// at an offset that does not fit one either.
auto tBufferPastTwoGigabytes =
    test("LargeBuffer/aBufferPastTwoGigabytesAddressesItsEnd") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto big = device.makeBuffer(largeBufferBytes, BufferUsage::Storage);

    // A device that will not give us the allocation says so by handing back an
    // invalid buffer, and that is a skip: the arithmetic is the subject here.
    if (!big.isValid())
        return;

    check(big.size() == largeBufferBytes);

    const auto tailOffset = largeBufferBytes - (std::int64_t) sizeof(tailMarker);

    check(tailOffset > (std::int64_t) INT_MAX);

    big.update(&headMarker, (std::int64_t) sizeof(headMarker));
    big.update(&tailMarker, (std::int64_t) sizeof(tailMarker), tailOffset);

    auto head = std::uint32_t {};
    auto tail = std::uint32_t {};

    big.read(&head, (std::int64_t) sizeof(head));
    big.read(&tail, (std::int64_t) sizeof(tail), tailOffset);

    // The tail write landing anywhere else - which is what a wrapped offset
    // does - leaves the head as it was and the tail as it never was.
    check(head == headMarker);
    check(tail == tailMarker);

    // And the range over that tail names the same bytes the read did.
    const auto range =
        BufferRange {&big, tailOffset, (std::int64_t) sizeof(tailMarker)};

    check(range.isValid());
    check(range.offset + range.bytes == big.size());
};

// An offset past the buffer's end still reads and writes nothing, which is the
// guard the wider type had to keep rather than lose.
auto tOffsetsPastTheEndAreStillRefused =
    test("LargeBuffer/anOffsetPastTheEndIsStillRefused") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const std::uint32_t source[] = {1u, 2u, 3u, 4u};

    auto buffer = device.makeBuffer(source, BufferUsage::Storage);

    check(buffer.size() == (std::int64_t) sizeof(source));

    const std::uint32_t overwrite = 0xffffffffu;

    buffer.update(&overwrite, (std::int64_t) sizeof(overwrite), buffer.size());
    buffer.update(&overwrite, (std::int64_t) sizeof(overwrite), -1);
    buffer.update(&overwrite, (std::int64_t) sizeof(overwrite), largeBufferBytes);

    std::uint32_t values[4] = {};
    buffer.read(values, (std::int64_t) sizeof(values));

    for (auto i = 0; i < 4; ++i)
        check(values[i] == source[i]);
};
