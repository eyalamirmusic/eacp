#include "Base64.h"

namespace eacp::Base64
{

namespace
{
constexpr auto alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string encode(std::span<const std::uint8_t> bytes)
{
    auto out = std::string {};
    out.reserve(((bytes.size() + 2) / 3) * 4);

    auto i = std::size_t {0};
    for (; i + 3 <= bytes.size(); i += 3)
    {
        auto chunk = (std::uint32_t {bytes[i]} << 16)
                     | (std::uint32_t {bytes[i + 1]} << 8)
                     | std::uint32_t {bytes[i + 2]};

        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out.push_back(alphabet[(chunk >> 6) & 0x3f]);
        out.push_back(alphabet[chunk & 0x3f]);
    }

    auto remaining = bytes.size() - i;
    if (remaining == 1)
    {
        auto chunk = std::uint32_t {bytes[i]} << 16;
        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out += "==";
    }
    else if (remaining == 2)
    {
        auto chunk =
            (std::uint32_t {bytes[i]} << 16) | (std::uint32_t {bytes[i + 1]} << 8);
        out.push_back(alphabet[(chunk >> 18) & 0x3f]);
        out.push_back(alphabet[(chunk >> 12) & 0x3f]);
        out.push_back(alphabet[(chunk >> 6) & 0x3f]);
        out.push_back('=');
    }

    return out;
}

std::string encode(std::string_view text)
{
    return encode(
        std::span {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

} // namespace eacp::Base64
