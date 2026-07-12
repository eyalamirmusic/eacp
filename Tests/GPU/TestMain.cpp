#include "Common.h"

namespace
{
int argCount = 0;
char** argValues = nullptr;
int exitCode = 0;

void runTests()
{
    exitCode = nano::run(argCount, argValues);
}
} // namespace

int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;

    eacp::Apps::run(runTests);
    return exitCode;
}
