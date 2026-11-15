// Fixture for ModuleInfoTests: prints this executable's role and application
// path, after `--chdir <directory>` when given one.
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
