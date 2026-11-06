#pragma once

/** @file SHA-256 digests of bytes, text and files. */

#include "Common.h"
#include "FilePath.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace eacp
{
constexpr auto sha256DigestSize = 32;
constexpr auto sha256BlockSize = 64;
constexpr auto sha256WordCount = 8;

using Sha256Digest = std::array<std::uint8_t, sha256DigestSize>;

// SHA-256 (FIPS 180-4) over bytes fed in any number of pieces. finish() pads
// and returns the digest; the object is spent after it.
class Sha256
{
public:
    Sha256();

    void update(Span<const std::uint8_t> bytes);
    void update(std::string_view text);
    Sha256Digest finish();

private:
    void processBlock(std::span<const std::uint8_t, sha256BlockSize> block);

    std::array<std::uint32_t, sha256WordCount> state {};
    std::array<std::uint8_t, sha256BlockSize> pending {};
    std::size_t pendingSize = 0;
    std::uint64_t totalBytes = 0;
};

// Lowercase hex of the digest of text.
std::string computeSha256Hex(std::string_view text);

// The digest of the file's bytes, read through a MemoryMappedFile. nullopt
// when the file cannot be opened.
std::optional<Sha256Digest> computeFileSha256(const FilePath& file);

// Lowercase hex of computeFileSha256, empty when the file cannot be opened.
std::string computeFileSha256Hex(const FilePath& file);
} // namespace eacp
