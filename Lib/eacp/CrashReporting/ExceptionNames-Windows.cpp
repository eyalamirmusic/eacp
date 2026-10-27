#include "ExceptionNames-Windows.h"
#include "CppExceptions-Windows.h"

#include <array>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto heapCorruptionCode = DWORD {0xC0000374};
constexpr auto stackBufferOverrunCode = DWORD {0xC0000409};

struct ExceptionName
{
    DWORD code;
    std::string_view name;
};

constexpr auto exceptionNames = std::to_array<ExceptionName>({
    {EXCEPTION_ACCESS_VIOLATION, "EXCEPTION_ACCESS_VIOLATION"},
    {EXCEPTION_ARRAY_BOUNDS_EXCEEDED, "EXCEPTION_ARRAY_BOUNDS_EXCEEDED"},
    {EXCEPTION_BREAKPOINT, "EXCEPTION_BREAKPOINT"},
    {EXCEPTION_DATATYPE_MISALIGNMENT, "EXCEPTION_DATATYPE_MISALIGNMENT"},
    {EXCEPTION_FLT_DIVIDE_BY_ZERO, "EXCEPTION_FLT_DIVIDE_BY_ZERO"},
    {EXCEPTION_ILLEGAL_INSTRUCTION, "EXCEPTION_ILLEGAL_INSTRUCTION"},
    {EXCEPTION_IN_PAGE_ERROR, "EXCEPTION_IN_PAGE_ERROR"},
    {EXCEPTION_INT_DIVIDE_BY_ZERO, "EXCEPTION_INT_DIVIDE_BY_ZERO"},
    {EXCEPTION_PRIV_INSTRUCTION, "EXCEPTION_PRIV_INSTRUCTION"},
    {EXCEPTION_STACK_OVERFLOW, "EXCEPTION_STACK_OVERFLOW"},
    {heapCorruptionCode, "STATUS_HEAP_CORRUPTION"},
    {stackBufferOverrunCode, "STATUS_STACK_BUFFER_OVERRUN"},
    {cppExceptionCode, "C++ exception"},
});

} // namespace

std::optional<std::string_view> findExceptionName(DWORD code)
{
    for (const auto& entry: exceptionNames)
    {
        if (entry.code == code)
            return entry.name;
    }

    return std::nullopt;
}
} // namespace eacp::CrashReporting::Detail
