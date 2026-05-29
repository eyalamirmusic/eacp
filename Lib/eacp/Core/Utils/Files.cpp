#include "Files.h"
#include <fstream>
#include <sstream>
#include <system_error>

namespace eacp::Files
{
std::string readFile(const std::string& path)
{
    auto stream = std::ifstream(path);

    if (!stream.is_open())
        return {};

    auto buffer = std::ostringstream();
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string filenameFromPath(const std::string& path)
{
    auto separator = path.find_last_of("/\\");

    if (separator != std::string::npos)
        return path.substr(separator + 1);

    return path;
}

bool isUnderRoot(const std::filesystem::path& file,
                 const std::filesystem::path& root)
{
    auto ec = std::error_code {};
    auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);

    if (ec)
        return false;

    auto canonicalFile = std::filesystem::weakly_canonical(file, ec);

    if (ec)
        return false;

    auto rel = std::filesystem::relative(canonicalFile, canonicalRoot, ec);

    if (ec || rel.empty() || rel.is_absolute())
        return false;

    auto first = rel.begin();
    return first == rel.end() || *first != "..";
}
} // namespace eacp::Files
