#include "Sha256.h"

#include "MemoryMappedFile.h"
#include "Strings.h"

#include <algorithm>
#include <vector>

namespace eacp
{
namespace
{
constexpr auto roundConstants = std::array<std::uint32_t, 64> {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr auto initialState =
    std::array<std::uint32_t, sha256WordCount> {0x6a09e667,
                                                0xbb67ae85,
                                                0x3c6ef372,
                                                0xa54ff53a,
                                                0x510e527f,
                                                0x9b05688c,
                                                0x1f83d9ab,
                                                0x5be0cd19};

constexpr auto messageScheduleSize = 64;
constexpr auto lengthFieldSize = 8;
constexpr auto bitsPerByte = 8;
constexpr auto bytesPerWord = 4;
constexpr auto blockWordCount = sha256BlockSize / bytesPerWord;
constexpr auto paddingStartByte = std::uint8_t {0x80};

std::uint32_t rotateRight(std::uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

std::uint32_t readBigEndianWord(std::span<const std::uint8_t, bytesPerWord> bytes)
{
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16)
           | (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
}
} // namespace

Sha256::Sha256()
    : state(initialState)
{
}

void Sha256::processBlock(std::span<const std::uint8_t, sha256BlockSize> block)
{
    auto schedule = std::array<std::uint32_t, messageScheduleSize> {};

    for (auto i = 0; i < blockWordCount; ++i)
        schedule[i] =
            readBigEndianWord(block.subspan(i * bytesPerWord).first<bytesPerWord>());

    for (auto i = blockWordCount; i < messageScheduleSize; ++i)
    {
        auto s0 = rotateRight(schedule[i - 15], 7)
                  ^ rotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        auto s1 = rotateRight(schedule[i - 2], 17) ^ rotateRight(schedule[i - 2], 19)
                  ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;

    for (auto i = 0; i < messageScheduleSize; ++i)
    {
        auto sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        auto choice = (e & f) ^ (~e & g);
        auto temp1 = h + sum1 + choice + roundConstants[i] + schedule[i];
        auto sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        auto majority = (a & b) ^ (a & c) ^ (b & c);
        auto temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void Sha256::update(Span<const std::uint8_t> bytesToAdd)
{
    auto bytes = bytesToAdd.getSpan();
    totalBytes += bytes.size();
    auto offset = std::size_t {0};

    if (pendingSize > 0)
    {
        auto toCopy = std::min(sha256BlockSize - pendingSize, bytes.size());
        std::copy_n(bytes.data(), toCopy, pending.data() + pendingSize);
        pendingSize += toCopy;
        offset = toCopy;

        if (pendingSize < sha256BlockSize)
            return;

        processBlock(pending);
        pendingSize = 0;
    }

    while (bytes.size() - offset >= sha256BlockSize)
    {
        processBlock(bytes.subspan(offset).first<sha256BlockSize>());
        offset += sha256BlockSize;
    }

    pendingSize = bytes.size() - offset;
    std::copy_n(bytes.data() + offset, pendingSize, pending.data());
}

void Sha256::update(std::string_view text)
{
    auto bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    update(Span<const std::uint8_t>(bytes, text.size()));
}

Sha256Digest Sha256::finish()
{
    auto totalBits = totalBytes * bitsPerByte;
    auto padding = std::vector<std::uint8_t> {paddingStartByte};
    auto lengthOffset = sha256BlockSize - lengthFieldSize;

    while ((pendingSize + padding.size()) % sha256BlockSize != lengthOffset)
        padding.push_back(0);

    for (auto i = lengthFieldSize - 1; i >= 0; --i)
        padding.push_back(std::uint8_t(totalBits >> (i * bitsPerByte)));

    auto savedTotal = totalBytes;
    update(Span<const std::uint8_t> {padding});
    totalBytes = savedTotal;

    auto digest = Sha256Digest {};

    for (auto i = 0; i < sha256WordCount; ++i)
    {
        for (auto byte = 0; byte < bytesPerWord; ++byte)
        {
            auto shift = (bytesPerWord - 1 - byte) * bitsPerByte;
            digest[i * bytesPerWord + byte] = std::uint8_t(state[i] >> shift);
        }
    }

    return digest;
}

std::string computeSha256Hex(std::string_view text)
{
    auto hasher = Sha256 {};
    hasher.update(text);
    const auto digest = hasher.finish();
    return Strings::toHex(digest);
}

std::optional<Sha256Digest> computeFileSha256(const FilePath& file)
{
    auto mapped = MemoryMappedFile {file};

    if (!mapped.isValid())
        return std::nullopt;

    auto hasher = Sha256 {};
    hasher.update(mapped.bytes());
    return hasher.finish();
}

std::string computeFileSha256Hex(const FilePath& file)
{
    auto digest = computeFileSha256(file);

    if (!digest)
        return {};

    return Strings::toHex(*digest);
}
} // namespace eacp
