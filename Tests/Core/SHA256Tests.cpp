#include <eacp/Core/Utils/SHA256.h>

#include <NanoTest/NanoTest.h>

#include <filesystem>
#include <fstream>

using namespace nano;

namespace
{
void writeFile(const std::filesystem::path& path, const std::string& text)
{
    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    out << text;
}
} // namespace

auto tSha256EmptyKnownVector = test("SHA256/stringEmptyKnownVector") = []
{
    check(eacp::Crypto::sha256String("")
          == "e3b0c44298fc1c149afbf4c8996fb924"
             "27ae41e4649b934ca495991b7852b855");
};

auto tSha256StringKnownVector = test("SHA256/stringKnownVector") = []
{
    check(eacp::Crypto::sha256String("abc")
          == "ba7816bf8f01cfea414140de5dae2223"
             "b00361a396177a9cb410ff61f20015ad");
};

auto tSha256IncrementalMatchesOneShot =
    test("SHA256/incrementalMatchesOneShot") = []
{
    auto sha = eacp::Crypto::SHA256();
    sha.update("a");
    sha.update("b");
    sha.update("c");

    check(sha.finish() == eacp::Crypto::sha256String("abc"));
};

auto tSha256FinishIsIdempotent = test("SHA256/finishIsIdempotent") = []
{
    auto sha = eacp::Crypto::SHA256();
    sha.update("abc");

    auto first = sha.finish();
    auto second = sha.finish();

    check(first == second);
    check(first == eacp::Crypto::sha256String("abc"));
};

auto tSha256FileKnownVector = test("SHA256/fileKnownVector") = []
{
    auto path = std::filesystem::temp_directory_path() / "eacp-sha256-test.txt";
    writeFile(path, "abc");

    check(eacp::Crypto::sha256File(path.string())
          == eacp::Crypto::sha256String("abc"));
};
