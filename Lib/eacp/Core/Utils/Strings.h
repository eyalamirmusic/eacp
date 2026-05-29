#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace eacp::Strings
{
std::string trim(const std::string& s);
std::string toLower(const std::string& s);

bool equalsCaseInsensitive(const std::string& a, const std::string& b);

int hexCharToInt(char c);
std::string percentDecode(std::string_view encoded);

std::optional<float> tryParseFloat(const std::string& s);
std::optional<int> tryParseInt(const std::string& s);

float parseFloatOr(const std::string& s, float fallback = 0.f);
int parseIntOr(const std::string& s, int fallback = 0);
} // namespace eacp::Strings
