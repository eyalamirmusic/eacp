#include "Files.h"
#include "StdPath.h"

#include <fstream>
#include <sstream>

namespace eacp::Files
{
std::string readFile(const FilePath& path)
{
    auto stream = std::ifstream(toStdPath(path));

    if (!stream.is_open())
        return {};

    auto buffer = std::ostringstream();
    buffer << stream.rdbuf();
    return buffer.str();
}

void writeFile(const FilePath& path, std::span<const std::uint8_t> bytes)
{
    auto fsPath = toStdPath(path);

    if (fsPath.has_parent_path())
        std::filesystem::create_directories(fsPath.parent_path());

    auto stream = std::ofstream(fsPath, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw std::runtime_error("cannot open '" + path.str() + "' for writing");

    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));

    if (!stream)
        throw std::runtime_error("cannot write '" + path.str() + "'");
}

std::string filenameFromPath(const std::string& path)
{
    auto separator = path.find_last_of("/\\");

    if (separator != std::string::npos)
        return path.substr(separator + 1);

    return path;
}
} // namespace eacp::Files
