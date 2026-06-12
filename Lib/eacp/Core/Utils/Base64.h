#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace eacp::Base64
{

// Standard RFC 4648 base64 with padding — the alphabet data URIs and
// JSON-embedded binary payloads (e.g. MCP image content) expect.
std::string encode(std::span<const std::uint8_t> bytes);
std::string encode(std::string_view text);

} // namespace eacp::Base64
