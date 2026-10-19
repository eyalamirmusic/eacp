#include "Console.h"

#include <cstdio>

namespace eacp::Console
{
namespace
{
void writeLine(std::FILE* stream, std::string_view line)
{
    std::fwrite(line.data(), 1, line.size(), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}
} // namespace

void print(std::string_view line)
{
    writeLine(stdout, line);
}

void printError(std::string_view line)
{
    writeLine(stderr, line);
}
} // namespace eacp::Console
