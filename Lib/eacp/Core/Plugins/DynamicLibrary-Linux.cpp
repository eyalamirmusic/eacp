#include "DynamicLibrary.h"

namespace eacp::Plugins
{
// No ELF symbol walk yet — enumeration is Mach-O-only for now (see
// DynamicLibrary-Apple.cpp); findSymbol with a known name still works.
Vector<std::string> DynamicLibrary::getFunctionNames() const
{
    return {};
}
} // namespace eacp::Plugins
