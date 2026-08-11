#include "DynamicLibrary.h"

namespace eacp::Plugins
{
// No ELF symbol walk yet; findSymbol with a known name still works.
Vector<std::string> DynamicLibrary::getFunctionNames() const
{
    return {};
}
} // namespace eacp::Plugins
