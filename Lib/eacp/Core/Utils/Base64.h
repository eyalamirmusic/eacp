#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace eacp::Base64
{
// RFC 4648 §4, padded. Bytes in, ASCII out - what a data URI, an HTTP Basic
// header or the WebSocket handshake's accept key all want.
std::string encode(std::string_view bytes);

// The reverse. Nullopt on input that is not valid base64: a character outside
// the alphabet, a length that is not a multiple of four, or padding anywhere
// but the last two positions.
std::optional<std::string> decode(std::string_view text);
} // namespace eacp::Base64
