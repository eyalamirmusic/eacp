#include "Files.h"
#include <fstream>
#include <sstream>

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
} // namespace eacp::Files
