#include "Embed.h"

#include <eacp/Core/Core.h>

#include <iomanip>
#include <sstream>

namespace eacp::ResourceEmbed
{
std::string generateEmbedCode(const std::string& source,
                              const std::string& resourceName)
{
    auto bytes = reinterpret_cast<const unsigned char*>(source.data());
    auto size = source.size();

    auto out = std::ostringstream();
    out << "#pragma once\n";
    out << "#include <array>\n";
    out << "#include <cstddef>\n";
    out << "\n";
    out << "namespace eacp::Resources\n";
    out << "{\n";
    out << "inline constexpr std::array<unsigned char, " << size << "> "
        << resourceName << " = {\n";

    for (std::size_t i = 0; i < size; ++i)
    {
        if (i % 12 == 0)
            out << "    ";

        out << "0x" << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<unsigned int>(bytes[i]);

        if (i + 1 < size)
            out << ", ";

        if (i % 12 == 11)
            out << "\n";
    }

    if (size > 0 && size % 12 != 0)
        out << "\n";

    out << "};\n";
    out << "} // namespace eacp::Resources\n";

    return out.str();
}

std::string generateEmbedCodeFromFile(const std::string& inputPath,
                                      const std::string& resourceName)
{
    auto fileAsString = Files::readFile(inputPath);
    return generateEmbedCode(fileAsString, resourceName);
}
} // namespace eacp::ResourceEmbed
