#pragma once

#include <string>

namespace eacp::CrashReporting::Detail
{
// The C++ name a mangled symbol stands for; the symbol itself when it is not
// one.
std::string demangleSymbol(const char* symbol);
} // namespace eacp::CrashReporting::Detail
