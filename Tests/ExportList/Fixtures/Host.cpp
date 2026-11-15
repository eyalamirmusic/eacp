#include <eacp/Core/Utils/WinInclude.h>

#include <cstdio>

// Exports the fixture archive through the list eacp_add_export_list wrote, then
// loads the module given as the first argument and prints what it answers.
int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;

    auto module = LoadLibraryA(argv[1]);

    if (module == nullptr)
    {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    using Entry = int (*)(int);
    auto entry = reinterpret_cast<Entry>(
        reinterpret_cast<void*>(GetProcAddress(module, "exportListModuleEntry")));

    if (entry == nullptr)
        return 1;

    std::printf("exportListModuleEntry(2) = %d\n", entry(2));
    return 0;
}
