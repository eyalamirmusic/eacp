#include <eacp/Core/App/App.h>

#include <NanoTest/NanoTest.h>

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

    eacp::Apps::getAppEnvironment().headless = true;

    eacp::Apps::run(runTests);
    return exitCode;
}
