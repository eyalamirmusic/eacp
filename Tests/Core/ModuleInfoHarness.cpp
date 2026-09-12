// Fixture for ModuleInfoTests: prints how this executable classifies its own
// image. A separate process because the answer depends on the path the program
// was launched with, and only a launcher can vary that.
#include <eacp/Core/Plugins/ModuleInfo.h>

#include <cstdio>

int main()
{
    std::printf("%s\n", eacp::Plugins::isDynamicLibrary() ? "plugin" : "standalone");

    return 0;
}
