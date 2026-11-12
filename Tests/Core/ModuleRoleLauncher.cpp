// Fixture for ModuleRoleTests: a thin launcher with no eacp copy of its own.
// It links ModuleRoleLibraryA and, unless built as ModuleRoleLauncherUnclaimed
// (EACP_MODULE_ROLE_UNCLAIMED), claims that library's copy. It prints what the
// linked copy answers as key=value lines, then, given a library path as its
// argument, loads that library at run time and prints what its copy answers.
#include <eacp/Core/Plugins/ClaimCopy.h>

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

EACP_PLUGIN_EXTERN_C const void* eacpModuleRoleAddress();
EACP_PLUGIN_EXTERN_C int eacpModuleRoleIsDLL();
EACP_PLUGIN_EXTERN_C int eacpModuleRoleIsDLLAtLoad();
EACP_PLUGIN_EXTERN_C const char* eacpModuleRoleCopyPath();
EACP_PLUGIN_EXTERN_C const char* eacpModuleRoleProductPath();
EACP_PLUGIN_EXTERN_C const char* eacpModuleRoleApplicationPath();

#ifndef EACP_MODULE_ROLE_UNCLAIMED
EACP_CLAIM_COPY(eacpModuleRoleAddress());
#endif

namespace
{
using IntFunction = int (*)();
using TextFunction = const char* (*) ();

void* openLibrary(const char* path)
{
#ifdef _WIN32
    return (void*) LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* findSymbol(void* library, const char* name)
{
#ifdef _WIN32
    return (void*) GetProcAddress((HMODULE) library, name);
#else
    return dlsym(library, name);
#endif
}

int reportLoadedLibrary(const char* path)
{
    auto* library = openLibrary(path);

    if (library == nullptr)
        return 2;

    auto isDLL =
        reinterpret_cast<IntFunction>(findSymbol(library, "eacpModuleRoleIsDLL"));
    auto product = reinterpret_cast<TextFunction>(
        findSymbol(library, "eacpModuleRoleProductPath"));
    auto copy = reinterpret_cast<TextFunction>(
        findSymbol(library, "eacpModuleRoleCopyPath"));

    if (isDLL == nullptr || product == nullptr || copy == nullptr)
        return 3;

    std::printf("loaded.isDLL=%d\n", isDLL());
    std::printf("loaded.product=%s\n", product());
    std::printf("loaded.copy=%s\n", copy());

    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    std::printf("linked.isDLL=%d\n", eacpModuleRoleIsDLL());
    std::printf("linked.isDLLAtLoad=%d\n", eacpModuleRoleIsDLLAtLoad());
    std::printf("linked.copy=%s\n", eacpModuleRoleCopyPath());
    std::printf("linked.product=%s\n", eacpModuleRoleProductPath());
    std::printf("linked.application=%s\n", eacpModuleRoleApplicationPath());

    if (argc > 1)
        return reportLoadedLibrary(argv[1]);

    return 0;
}
