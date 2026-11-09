#include "ProcessChildModes.h"

#include <NanoTest/NanoTest.h>

int main(int argc, char** argv)
{
    if (ProcessChildModes::isChildModeRequested(argc, argv))
        return ProcessChildModes::runChildMode(argc, argv);

    return nano::run(argc, argv);
}
