#include "CrashingProcess.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/CrashReporting/CrashReporting.h>

#include <NanoTest/NanoTest.h>

using namespace eacp;

int main(int argc, char* argv[])
{
    if (auto code = CrashReporting::runHandlerIfRequested(argc, argv))
        return *code;

    if (CrashReporting::Tests::isCrashingProcessCommandLine(argc, argv))
    {
        Apps::setCommandLineArgs(argc, argv);
        return CrashReporting::Tests::runCrashingProcess();
    }

    return nano::run(argc, argv);
}
