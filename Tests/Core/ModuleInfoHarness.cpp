// Fixture for ModuleInfoTests: prints how this executable classifies its own
// image, then the application path it reports. A separate process because both
// once depended on how the program was launched, and only a launcher can vary
// that. `--chdir <directory>` moves to the directory before asking anything.
#include <eacp/Core/Plugins/ModuleInfo.h>
#include <eacp/Core/Utils/Files.h>

#include <cstdio>
#include <filesystem>
#include <string_view>

int main(int argc, char** argv)
{
    if (argc > 2 && std::string_view {argv[1]} == "--chdir")
        std::filesystem::current_path(argv[2]);

    std::printf("%s\n", eacp::Plugins::isDynamicLibrary() ? "plugin" : "standalone");
    std::printf("%s\n", eacp::Files::applicationPath().c_str());

    return 0;
}
