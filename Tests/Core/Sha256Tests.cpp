#include "Common.h"

#include <eacp/Core/Utils/Sha256.h>

using namespace nano;

constexpr auto nistMillionCharacterCount = 1'000'000;

auto tSha256Empty = test("Sha256/empty message") = []
{
    check(eacp::computeSha256Hex("")
          == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
};

auto tSha256Abc = test("Sha256/abc") = []
{
    check(eacp::computeSha256Hex("abc")
          == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
};

auto tSha256TwoBlockMessage = test("Sha256/448-bit message") = []
{
    check(eacp::computeSha256Hex(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
};

auto tSha256MillionA = test("Sha256/one million a") = []
{
    check(eacp::computeSha256Hex(std::string(nistMillionCharacterCount, 'a'))
          == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
};

auto tSha256PiecesMatchWhole =
    test("Sha256/update in pieces matches one update") = []
{
    constexpr auto pieceSize = 7;
    const auto text = std::string(nistMillionCharacterCount, 'a');

    auto hasher = eacp::Sha256 {};

    for (auto offset = std::size_t {0}; offset < text.size(); offset += pieceSize)
        hasher.update(std::string_view {text}.substr(offset, pieceSize));

    const auto digest = hasher.finish();
    check(eacp::Strings::toHex(digest) == eacp::computeSha256Hex(text));
};

auto tSha256File = test("Sha256/file digest matches text digest") = []
{
    auto dir = scratchDirectory("sha256-file");
    auto path = dir / "abc.txt";
    writeTestFile(path, "abc");

    check(eacp::computeFileSha256Hex(path) == eacp::computeSha256Hex("abc"));

    auto empty = dir / "empty.txt";
    writeTestFile(empty, "");
    check(eacp::computeFileSha256Hex(empty) == eacp::computeSha256Hex(""));

    std::filesystem::remove_all(dir);
};

auto tSha256MissingFile = test("Sha256/missing file has no digest") = []
{
    auto dir = scratchDirectory("sha256-missing");

    check(!eacp::computeFileSha256(dir / "nothing.bin").has_value());
    check(eacp::computeFileSha256Hex(dir / "nothing.bin").empty());

    std::filesystem::remove_all(dir);
};
