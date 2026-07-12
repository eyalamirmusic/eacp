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

// Window construction needs the app environment initLoopThread() sets up
// (on Windows: the COM apartment), so the tests run inside Apps::run like
// the GPU tests do.
int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;

    eacp::Apps::getAppEnvironment().headless = true;

    eacp::Apps::run(runTests);
    return exitCode;
}
