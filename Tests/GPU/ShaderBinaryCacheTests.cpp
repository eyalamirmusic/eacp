#include "Common.h"

#include <eacp/GPU/Shader/ShaderBinaryCache.h>

// ShaderBinaryCache - compiled shader bytes kept on disk between runs, found by
// the compiler that made them and everything the compile read.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// Bytes a text-mode read or write would mangle: a NUL, a CRLF, a lone CR and
// every byte value above 127.
std::string awkwardBytes()
{
    auto bytes = std::string {"DXBC\0\r\n\r", 8};

    for (auto value = 128; value < 256; ++value)
        bytes += (char) value;

    return bytes;
}

} // namespace

auto tShaderBinaryRoundTrip = test("ShaderBinaryCache/storedBytesComeBackWhole") = []
{
    ShaderBinaryCache::store("roundtrip-compiler", "source text", awkwardBytes());

    auto loaded = ShaderBinaryCache::load("roundtrip-compiler", "source text");

    check(loaded.has_value());
    check(loaded.has_value() && *loaded == awkwardBytes());
};

auto tShaderBinaryMisses =
    test("ShaderBinaryCache/anotherCompilerOrSourceMisses") = []
{
    ShaderBinaryCache::store("test-compiler-1", "source text", "bytes");

    check(!ShaderBinaryCache::load("test-compiler-2", "source text"));
    check(!ShaderBinaryCache::load("test-compiler-1", "source text 2"));
    check(!ShaderBinaryCache::load("test-compiler", "-1source text"));
};

auto tShaderBinaryLatestWins =
    test("ShaderBinaryCache/aSecondStoreReplacesTheFirst") = []
{
    ShaderBinaryCache::store("test-compiler-1", "latest source", "old");
    ShaderBinaryCache::store("test-compiler-1", "latest source", "new");

    auto loaded = ShaderBinaryCache::load("test-compiler-1", "latest source");
    check(loaded.has_value() && *loaded == "new");
};
