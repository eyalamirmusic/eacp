#include "ModuleInfo.h"
#include "ModuleInfoPlatform.h"
#include "ModuleInfoPosix.h"

#include "../Platform/Platform.h"
#include "../Utils/Files.h"

#include <algorithm>
#include <dlfcn.h>
#include <link.h>
#include <span>

namespace eacp::Plugins
{
namespace
{
struct MainProgramProbe
{
    ElfW(Addr) address = 0;
    bool reported = false;
    bool contains = false;
};

bool loadSegmentContains(const dl_phdr_info& image,
                         const ElfW(Phdr) & header,
                         ElfW(Addr) address)
{
    const auto start = image.dlpi_addr + header.p_vaddr;

    return header.p_type == PT_LOAD && address >= start
           && address - start < header.p_memsz;
}

// glibc, musl and bionic all report the main program first, so the first call
// is the only one looked at.
int probeMainProgram(dl_phdr_info* image, std::size_t, void* data)
{
    auto& probe = *(MainProgramProbe*) data;
    const auto headers =
        std::span {image->dlpi_phdr, (std::size_t) image->dlpi_phnum};

    probe.reported = true;
    probe.contains =
        std::any_of(headers.begin(),
                    headers.end(),
                    [&](const ElfW(Phdr) & header)
                    { return loadSegmentContains(*image, header, probe.address); });

    return 1;
}
} // namespace

bool Detail::isInMainProgram(const void* address)
{
    auto probe = MainProgramProbe {(ElfW(Addr)) address};
    dl_iterate_phdr(probeMainProgram, &probe);

    return !probe.reported || probe.contains;
}

// RTLD_DEFAULT searches the whole global scope; ModuleInfo.cpp then refuses a
// definition that is not the executable's own.
void* Detail::findClaimFunction()
{
    return dlsym(RTLD_DEFAULT, "eacpClaimedCopy");
}

// For the main program glibc's dladdr reports argv[0], which names the binary
// only for some launches (not a bare name found on PATH, not a rewritten
// argv[0]), so the kernel answers there. Android's main program is
// app_process, never an eacp copy.
FilePath getCurrentModulePath()
{
    if constexpr (!Platform::isAndroid())
    {
        if (Detail::isInMainProgram(getCurrentModuleAddress()))
        {
            if (auto executable = Files::executablePath(); !executable.empty())
                return executable;
        }
    }

    return Detail::loadedImagePath();
}
} // namespace eacp::Plugins
