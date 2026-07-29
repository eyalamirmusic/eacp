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

// Through Apps::run like the GPU suite, because the rendered cases drive a
// GPUView and a view needs the platform's run loop up even when nothing is
// shown on screen.
int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;

    eacp::Apps::run(runTests);
    return exitCode;
}
