#include "SHA256.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>

namespace eacp::Crypto
{
namespace
{
std::uint32_t rotr(std::uint32_t value, std::uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}
} // namespace

struct SHA256::State
{
    std::array<std::uint32_t, 8> words {
        0x6a09e667u,
        0xbb67ae85u,
        0x3c6ef372u,
        0xa54ff53au,
        0x510e527fu,
        0x9b05688cu,
        0x1f83d9abu,
        0x5be0cd19u};
    std::array<std::uint8_t, 64> buffer {};
    std::uint64_t bitLength = 0;
    std::size_t bufferLength = 0;
    std::optional<std::string> digest;

    void transform(const std::uint8_t* data)
    {
        static constexpr std::array<std::uint32_t, 64> k {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        auto w = std::array<std::uint32_t, 64> {};
        for (auto i = 0; i < 16; ++i)
        {
            w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24u)
                 | (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16u)
                 | (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8u)
                 | static_cast<std::uint32_t>(data[i * 4 + 3]);
        }

        for (auto i = 16; i < 64; ++i)
        {
            auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18)
                    ^ (w[i - 15] >> 3u);
            auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19)
                    ^ (w[i - 2] >> 10u);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        auto a = words[0];
        auto b = words[1];
        auto c = words[2];
        auto d = words[3];
        auto e = words[4];
        auto f = words[5];
        auto g = words[6];
        auto h = words[7];

        for (auto i = 0; i < 64; ++i)
        {
            auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            auto ch = (e & f) ^ (~e & g);
            auto temp1 = h + s1 + ch + k[i] + w[i];
            auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            auto maj = (a & b) ^ (a & c) ^ (b & c);
            auto temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        words[0] += a;
        words[1] += b;
        words[2] += c;
        words[3] += d;
        words[4] += e;
        words[5] += f;
        words[6] += g;
        words[7] += h;
    }
};

SHA256::SHA256()
    : state(std::make_unique<State>())
{
}

SHA256::~SHA256() = default;
SHA256::SHA256(SHA256&&) noexcept = default;
SHA256& SHA256::operator=(SHA256&&) noexcept = default;

void SHA256::update(std::span<const std::uint8_t> data)
{
    if (state->digest)
        return;

    for (auto byte: data)
    {
        state->buffer[state->bufferLength++] = byte;
        if (state->bufferLength == state->buffer.size())
        {
            state->transform(state->buffer.data());
            state->bitLength += 512;
            state->bufferLength = 0;
        }
    }
}

void SHA256::update(const std::string& text)
{
    update(std::span(reinterpret_cast<const std::uint8_t*>(text.data()),
                     text.size()));
}

std::string SHA256::finish()
{
    if (state->digest)
        return *state->digest;

    auto i = state->bufferLength;
    state->buffer[i++] = 0x80;

    if (i > 56)
    {
        while (i < 64)
            state->buffer[i++] = 0;
        state->transform(state->buffer.data());
        i = 0;
    }

    while (i < 56)
        state->buffer[i++] = 0;

    state->bitLength += state->bufferLength * 8;
    for (auto j = 0; j < 8; ++j)
        state->buffer[63 - j] =
            static_cast<std::uint8_t>(state->bitLength >> (j * 8));

    state->transform(state->buffer.data());
    auto out = std::ostringstream();
    out << std::hex << std::setfill('0');
    for (auto word: state->words)
        out << std::setw(8) << word;

    state->digest = out.str();
    return *state->digest;
}

std::string sha256String(const std::string& text)
{
    auto sha = SHA256();
    sha.update(text);
    return sha.finish();
}

std::string sha256File(const std::string& path)
{
    auto in = std::ifstream(path, std::ios::binary);
    if (!in)
        return {};

    auto sha = SHA256();
    auto buffer = std::array<std::uint8_t, 4096> {};
    while (in)
    {
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
        auto count = in.gcount();
        if (count > 0)
            sha.update(std::span(buffer.data(), static_cast<std::size_t>(count)));
    }

    return sha.finish();
}

} // namespace eacp::Crypto
