#include <eacp/Core/Utils/Hash.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using eacp::Hashing::fnv1a64;
using eacp::Hashing::toHex;

// The published FNV-1a 64 vectors: a build tool and a runtime on another
// machine have to agree on every one of them.
auto tFnv1a64Vectors = test("Hash/fnv1a64/publishedVectors") = []
{
    check(fnv1a64("") == 0xcbf29ce484222325ull);
    check(fnv1a64("a") == 0xaf63dc4c8601ec8cull);
    check(fnv1a64("foobar") == 0x85944171f73967e8ull);
};

auto tToHexPadsToSixteenDigits = test("Hash/toHex/padsToSixteenDigits") = []
{
    check(toHex(0) == "0000000000000000");
    check(toHex(0xabcull) == "0000000000000abc");
    check(toHex(0xcbf29ce484222325ull) == "cbf29ce484222325");
    check(toHex(~0ull) == "ffffffffffffffff");
};
