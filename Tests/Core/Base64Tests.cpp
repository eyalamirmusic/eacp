#include "Common.h"

#include <eacp/Core/Utils/Base64.h>

using namespace nano;
using eacp::Base64::decode;
using eacp::Base64::encode;

// RFC 4648 §10's vectors, which pin all three padding cases at once.
auto tEncodesRfcVectors = test("Base64/encode/rfcVectors") = []
{
    check(encode("") == "");
    check(encode("f") == "Zg==");
    check(encode("fo") == "Zm8=");
    check(encode("foo") == "Zm9v");
    check(encode("foob") == "Zm9vYg==");
    check(encode("fooba") == "Zm9vYmE=");
    check(encode("foobar") == "Zm9vYmFy");
};

auto tDecodesRfcVectors = test("Base64/decode/rfcVectors") = []
{
    check(decode("") == "");
    check(decode("Zg==") == "f");
    check(decode("Zm8=") == "fo");
    check(decode("Zm9v") == "foo");
    check(decode("Zm9vYg==") == "foob");
    check(decode("Zm9vYmE=") == "fooba");
    check(decode("Zm9vYmFy") == "foobar");
};

// The bytes an avatar or emoji upload carries: high bits set, embedded nulls,
// and the two alphabet entries a URL-safe variant would have spelled differently.
auto tRoundTripsBinary = test("Base64/roundTripsBinaryBytes") = []
{
    auto bytes = std::string();

    for (auto i = 0; i < 256; ++i)
        bytes.push_back((char) i);

    auto encoded = encode(bytes);

    check(encoded.find('+') != std::string::npos);
    check(encoded.find('/') != std::string::npos);
    check(decode(encoded) == bytes);
};

auto tEncodesEveryLength = test("Base64/roundTripsEveryLength") = []
{
    auto bytes = std::string();

    for (auto i = 0; i < 64; ++i)
    {
        check(encode(bytes).size() % 4 == 0);
        check(decode(encode(bytes)) == bytes);
        bytes.push_back((char) ('a' + i % 26));
    }
};

auto tRejectsBadLength = test("Base64/decode/rejectsUnpaddedLength") = []
{
    check(!decode("Zg").has_value());
    check(!decode("Zm9vYg").has_value());
    check(!decode("Zm9vYmFy=").has_value());
};

auto tRejectsBadCharacters = test("Base64/decode/rejectsForeignCharacters") = []
{
    check(!decode("Zm9-YmFy").has_value());
    check(!decode("Zm9 vYmFy").has_value());
    check(!decode("Zm9\nYmFy").has_value());
};

auto tRejectsInteriorPadding = test("Base64/decode/rejectsInteriorPadding") = []
{
    check(!decode("Zg==Zg==").has_value());
    check(!decode("Z=9v").has_value());
};
