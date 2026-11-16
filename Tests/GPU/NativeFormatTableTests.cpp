#include <eacp/GPU/NativeFormatTable.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp::GPU;

namespace NativeFormatTableTests
{
enum class PortableFormat
{
    Red,
    RedGreen,
    RedGreenBlue
};

enum class NativeFormat
{
    Unknown,
    NativeRed,
    NativeRedGreen,
    NativeRedGreenBlue
};

using Mapping = NativeFormatMapping<PortableFormat, NativeFormat>;

constexpr auto completeTable = std::to_array<Mapping>({
    {PortableFormat::Red, NativeFormat::NativeRed},
    {PortableFormat::RedGreen, NativeFormat::NativeRedGreen},
    {PortableFormat::RedGreenBlue, NativeFormat::NativeRedGreenBlue},
});

constexpr auto tableMissingAFormat = std::to_array<Mapping>({
    {PortableFormat::Red, NativeFormat::NativeRed},
    {PortableFormat::RedGreen, NativeFormat::NativeRedGreen},
});

constexpr auto tableListingAFormatTwice = std::to_array<Mapping>({
    {PortableFormat::Red, NativeFormat::NativeRed},
    {PortableFormat::RedGreen, NativeFormat::NativeRedGreen},
    {PortableFormat::RedGreen, NativeFormat::NativeRedGreen},
});

constexpr auto tableWithAnExtraDuplicate = std::to_array<Mapping>({
    {PortableFormat::Red, NativeFormat::NativeRed},
    {PortableFormat::RedGreen, NativeFormat::NativeRedGreen},
    {PortableFormat::RedGreenBlue, NativeFormat::NativeRedGreenBlue},
    {PortableFormat::Red, NativeFormat::NativeRed},
});

auto tComplete = test("NativeFormatTable/aCompleteTableMapsEveryFormatOnce") = []
{ check(mapsEveryFormatOnce(completeTable)); };

auto tMissing = test("NativeFormatTable/aMissingFormatIsRefused") = []
{ check(!mapsEveryFormatOnce(tableMissingAFormat)); };

auto tTwice = test("NativeFormatTable/aFormatListedTwiceIsRefused") = []
{
    check(!mapsEveryFormatOnce(tableListingAFormatTwice));
    check(!mapsEveryFormatOnce(tableWithAnExtraDuplicate));
};

auto tLookup = test("NativeFormatTable/lookupFindsTheMappedNativeFormat") = []
{
    check(findNativeFormat(
              completeTable, PortableFormat::RedGreen, NativeFormat::Unknown)
          == NativeFormat::NativeRedGreen);
};

auto tFallback = test("NativeFormatTable/anUnmappedFormatGetsTheFallback") = []
{
    check(findNativeFormat(tableMissingAFormat,
                           PortableFormat::RedGreenBlue,
                           NativeFormat::Unknown)
          == NativeFormat::Unknown);
};
} // namespace NativeFormatTableTests
