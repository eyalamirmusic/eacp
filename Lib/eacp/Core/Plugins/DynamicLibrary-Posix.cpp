#include "DynamicLibrary.h"

#include <cstring>
#include <dlfcn.h>
#include <utility>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#endif

namespace eacp::Plugins
{
DynamicLibrary::DynamicLibrary(const std::string& path)
{
    open(path);
}

DynamicLibrary::~DynamicLibrary()
{
    close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle(std::exchange(other.handle, nullptr))
{
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
{
    if (this != &other)
    {
        close();
        handle = std::exchange(other.handle, nullptr);
    }

    return *this;
}

bool DynamicLibrary::open(const std::string& path)
{
    close();
    handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
    return handle != nullptr;
}

void DynamicLibrary::close()
{
    if (handle != nullptr)
    {
        dlclose(handle);
        handle = nullptr;
    }
}

bool DynamicLibrary::isOpen() const
{
    return handle != nullptr;
}

void* DynamicLibrary::findSymbol(const std::string& name) const
{
    if (handle == nullptr)
        return nullptr;

    return dlsym(handle, name.c_str());
}

#ifdef __APPLE__
namespace
{
struct ImageLocation
{
    const mach_header_64* header = nullptr;
    intptr_t slide = 0;
};

// dlopen returns the same handle for an already-loaded image, so probing
// every image with RTLD_NOLOAD and comparing handles finds ours without
// fragile path canonicalisation.
ImageLocation findImage(void* handleToUse)
{
    for (auto i = uint32_t {0}; i < _dyld_image_count(); ++i)
    {
        auto* probe = dlopen(_dyld_get_image_name(i), RTLD_LAZY | RTLD_NOLOAD);

        if (probe == nullptr)
            continue;

        dlclose(probe);

        if (probe == handleToUse)
            return {(const mach_header_64*) _dyld_get_image_header(i),
                    _dyld_get_image_vmaddr_slide(i)};
    }

    return {};
}

bool isSegment(const char* segmentName, const char* expected)
{
    return std::strncmp(segmentName, expected, 16) == 0;
}
} // namespace

Vector<std::string> DynamicLibrary::getFunctionNames() const
{
    auto result = Vector<std::string>();
    auto image = findImage(handle);

    if (image.header == nullptr)
        return result;

    const symtab_command* symtab = nullptr;
    const segment_command_64* linkedit = nullptr;
    auto textSectionOrdinal = uint32_t {0};
    auto sectionOrdinal = uint32_t {0};

    auto* command = (const load_command*) (image.header + 1);

    for (auto i = uint32_t {0}; i < image.header->ncmds; ++i)
    {
        if (command->cmd == LC_SEGMENT_64)
        {
            auto* segment = (const segment_command_64*) command;

            if (isSegment(segment->segname, SEG_LINKEDIT))
                linkedit = segment;

            auto* sections = (const section_64*) (segment + 1);

            for (auto s = uint32_t {0}; s < segment->nsects; ++s)
            {
                ++sectionOrdinal;

                if (isSegment(sections[s].segname, SEG_TEXT)
                    && isSegment(sections[s].sectname, SECT_TEXT))
                    textSectionOrdinal = sectionOrdinal;
            }
        }
        else if (command->cmd == LC_SYMTAB)
        {
            symtab = (const symtab_command*) command;
        }

        command =
            (const load_command*) ((const uint8_t*) command + command->cmdsize);
    }

    if (symtab == nullptr || linkedit == nullptr || textSectionOrdinal == 0)
        return result;

    auto* linkeditBase = (const uint8_t*) (linkedit->vmaddr - linkedit->fileoff
                                           + (uintptr_t) image.slide);
    auto* symbols = (const nlist_64*) (linkeditBase + symtab->symoff);
    auto* strings = (const char*) (linkeditBase + symtab->stroff);

    for (auto i = uint32_t {0}; i < symtab->nsyms; ++i)
    {
        auto& symbol = symbols[i];

        if ((symbol.n_type & N_STAB) != 0 || (symbol.n_type & N_EXT) == 0
            || (symbol.n_type & N_TYPE) != N_SECT
            || symbol.n_sect != textSectionOrdinal)
            continue;

        auto* name = strings + symbol.n_un.n_strx;

        if (name[0] == '_')
            ++name;

        result.add(name);
    }

    return result;
}
#else
Vector<std::string> DynamicLibrary::getFunctionNames() const
{
    return {};
}
#endif
} // namespace eacp::Plugins
