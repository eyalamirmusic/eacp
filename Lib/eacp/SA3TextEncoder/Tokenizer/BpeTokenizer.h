#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace eacp::SA3TextEncoder
{
struct TokenizeResult
{
    std::vector<int> ids;
    int validLength = 0;
};

class BpeTokenizer
{
public:
    static std::optional<BpeTokenizer> load(const std::string& tokenizerJsonPath);

    TokenizeResult encode(const std::string& text, int maxLength) const;

private:
    BpeTokenizer() = default;

    std::vector<std::string> symbolsFor(const std::string& normalized) const;
    void mergeSymbols(std::vector<std::string>& symbols) const;
    int idFor(const std::string& symbol) const;

    std::unordered_map<std::string, int> vocab;
    std::unordered_map<std::string, int> mergeRank;
    int padTokenId = 0;
};
}
