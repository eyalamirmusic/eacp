#include "KernelName.h"

#if !defined(_MSC_VER)
#include <cxxabi.h>
#endif

#include <cstdlib>
#include <memory>

namespace eacp::GPU
{
namespace
{
std::string demangled(const std::type_info& type)
{
#if defined(_MSC_VER)
    // MSVC's name() is already readable, as "class eacp::ML::LinearF32".
    auto name = std::string {type.name()};

    for (auto prefix: {"class ", "struct "})
        if (name.rfind(prefix, 0) == 0)
            return name.substr(std::char_traits<char>::length(prefix));

    return name;
#else
    auto status = 0;
    auto text = std::unique_ptr<char, decltype(&std::free)> {
        abi::__cxa_demangle(type.name(), nullptr, nullptr, &status), &std::free};

    return status == 0 && text != nullptr ? std::string {text.get()}
                                          : std::string {type.name()};
#endif
}

// The last "::" outside any template brackets, so Scale<eacp::Float> keeps its
// argument whole.
std::string withoutNamespaces(const std::string& name)
{
    auto depth = 0;
    auto start = std::size_t {0};

    for (auto i = std::size_t {0}; i < name.size(); ++i)
    {
        if (name[i] == '<')
            ++depth;
        else if (name[i] == '>')
            --depth;
        else if (depth == 0 && name[i] == ':' && i + 1 < name.size()
                 && name[i + 1] == ':')
            start = i + 2;
    }

    return name.substr(start);
}
} // namespace

std::string readableTypeName(const std::type_info& type)
{
    return withoutNamespaces(demangled(type));
}
} // namespace eacp::GPU
