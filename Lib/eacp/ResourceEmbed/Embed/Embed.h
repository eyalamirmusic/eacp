#pragma once

#include <string>

namespace eacp::ResourceEmbed
{
std::string generateEmbedCode(const std::string& source,
                              const std::string& resourceName);

std::string generateEmbedCodeFromFile(const std::string& inputPath,
                                      const std::string& resourceName);
} // namespace eacp::ResourceEmbed
