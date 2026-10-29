#include "SymbolNames-macOS.h"

#include <cxxabi.h>

#include <cstdlib>
#include <memory>

namespace eacp::CrashReporting::Detail
{
std::string demangleSymbol(const char* symbol)
{
    auto status = 0;
    auto demangled = std::unique_ptr<char, decltype(&::free)> {
        abi::__cxa_demangle(symbol, nullptr, nullptr, &status), &::free};

    if (status != 0 || !demangled)
        return symbol;

    return demangled.get();
}
} // namespace eacp::CrashReporting::Detail
