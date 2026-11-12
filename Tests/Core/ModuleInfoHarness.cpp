// Fixture for ModuleInfoTests: prints how this executable classifies its own
// image, then the application path it reports. A separate process because both
// depend on the path the program was launched with, and only a launcher can vary
// that.
#include <eacp/Core/Plugins/ModuleInfo.h>
#include <eacp/Core/Utils/Files.h>

#include <cstdio>

int main()
{
    std::printf("%s\n", eacp::Plugins::isDynamicLibrary() ? "plugin" : "standalone");
    std::printf("%s\n", eacp::Files::applicationPath().str().c_str());

    return 0;
}
