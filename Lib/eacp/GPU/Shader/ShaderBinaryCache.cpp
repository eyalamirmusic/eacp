#include "ShaderBinaryCache.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/StdPath.h>

#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>

namespace eacp::GPU::ShaderBinaryCache
{
namespace
{
constexpr auto header = std::string_view {"eacp-shader-binary 1\n"};

std::uint64_t fnv1a(std::string_view text, std::uint64_t hash)
{
    for (auto character: text)
    {
        hash ^= (std::uint8_t) character;
        hash *= 1099511628211ull;
    }

    return hash;
}

std::string hex(std::uint64_t value)
{
    constexpr auto digits = "0123456789abcdef";
    auto text = std::string(16, '0');

    for (auto i = 15; i >= 0; --i, value >>= 4)
        text[(std::size_t) i] = digits[value & 0xf];

    return text;
}

// The compiler and the key, each with its length, so neither can run into the
// other.
std::string identity(std::string_view compiler, std::string_view key)
{
    return std::to_string(compiler.size()) + ':' + std::string {compiler}
           + std::to_string(key.size()) + ':' + std::string {key};
}

std::string readBytes(const FilePath& path)
{
    auto in = std::ifstream {toStdPath(path), std::ios::binary};
    return {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
}

FilePath entryPath(const std::string& entryIdentity)
{
    auto hash = fnv1a(entryIdentity, 14695981039346656037ull);
    return directory() / (hex(hash) + ".bin");
}
} // namespace

FilePath directory()
{
    return FilePath::appCacheDirectory() / "Shaders";
}

std::optional<std::string> load(std::string_view compiler, std::string_view key)
{
    try
    {
        auto expected = identity(compiler, key);
        auto contents = readBytes(entryPath(expected));
        auto prefix = std::string {header} + std::to_string(expected.size()) + '\n';

        if (contents.compare(0, prefix.size(), prefix) != 0)
            return std::nullopt;

        auto stored = std::string_view {contents}.substr(prefix.size());

        if (stored.substr(0, expected.size()) != expected)
            return std::nullopt;

        return std::string {stored.substr(expected.size())};
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

void store(std::string_view compiler, std::string_view key, std::string_view bytes)
{
    try
    {
        auto entryIdentity = identity(compiler, key);
        auto contents = std::string {header} + std::to_string(entryIdentity.size())
                        + '\n' + entryIdentity + std::string {bytes};

        Files::writeFileAtomically(
            entryPath(entryIdentity),
            Span<const std::uint8_t> {
                reinterpret_cast<const std::uint8_t*>(contents.data()),
                contents.size()});
    }
    catch (const std::exception&)
    {
    }
}
} // namespace eacp::GPU::ShaderBinaryCache
