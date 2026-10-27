#include "CppExceptions-Windows.h"

#include <dbghelp.h>

#include <array>
#include <cstring>
#include <exception>
#include <optional>

namespace eacp::CrashReporting::Detail
{
namespace
{
constexpr auto cppExceptionCode = DWORD {0xE06D7363};
constexpr auto cppExceptionMagic = ULONG_PTR {0x19930520};
constexpr auto cppExceptionParameterCount = DWORD {4};
constexpr auto thrownObjectParameter = 1;
constexpr auto throwInfoParameter = 2;
constexpr auto imageBaseParameter = 3;
constexpr auto typeNamePrefixLength = std::size_t {1};
constexpr auto stdExceptionTypeName = std::string_view {".?AVexception@std@@"};
constexpr auto maxUndecoratedTypeLength = std::size_t {512};
constexpr auto undecorateTypeOnly = DWORD {0x2800};
constexpr auto typeKeywords = std::to_array<std::string_view>({"class ", "struct "});

// The MSVC runtime's own layouts (ehdata.h), with image-relative offsets.
struct ThrowInfoLayout
{
    unsigned attributes;
    int unwindFunction;
    int forwardCompatibility;
    int catchableTypeArray;
};

struct CatchableTypeArrayLayout
{
    int count;
    int firstType;
};

struct CatchableTypeLayout
{
    unsigned properties;
    int typeDescriptor;
    int memberDisplacement;
    int virtualBaseDisplacement;
    int virtualDisplacement;
    int size;
    int copyFunction;
};

struct TypeDescriptorLayout
{
    const void* virtualTable;
    void* spare;
    char name[1];
};

template <typename T>
const T& atImageOffset(std::uintptr_t imageBase, int offset)
{
    return *reinterpret_cast<const T*>(imageBase
                                       + static_cast<std::uintptr_t>(offset));
}

struct ThrownType
{
    std::string_view decoratedName;
    int memberDisplacement;
};

template <typename Visit>
void forEachCatchableType(const EXCEPTION_RECORD& record, Visit visit)
{
    auto imageBase = record.ExceptionInformation[imageBaseParameter];
    const auto& throwInfo = *reinterpret_cast<const ThrowInfoLayout*>(
        record.ExceptionInformation[throwInfoParameter]);
    const auto& types = atImageOffset<CatchableTypeArrayLayout>(
        imageBase, throwInfo.catchableTypeArray);
    const auto* typeOffsets = &types.firstType;

    for (auto index = 0; index < types.count; ++index)
    {
        const auto& type =
            atImageOffset<CatchableTypeLayout>(imageBase, typeOffsets[index]);
        const auto& descriptor =
            atImageOffset<TypeDescriptorLayout>(imageBase, type.typeDescriptor);
        visit(ThrownType {descriptor.name, type.memberDisplacement});
    }
}

std::string_view removeTypeKeyword(std::string_view typeName)
{
    for (auto keyword: typeKeywords)
    {
        if (typeName.starts_with(keyword))
            return typeName.substr(keyword.size());
    }

    return typeName;
}

std::string_view
    undecorateTypeName(std::string_view decoratedName,
                       std::array<char, maxUndecoratedTypeLength>& buffer)
{
    if (decoratedName.size() <= typeNamePrefixLength)
        return decoratedName;

    auto length = ::UnDecorateSymbolName(decoratedName.data() + typeNamePrefixLength,
                                         buffer.data(),
                                         static_cast<DWORD>(buffer.size()),
                                         undecorateTypeOnly);

    if (length == 0)
        return decoratedName;

    return removeTypeKeyword({buffer.data(), length});
}
} // namespace

bool isCppException(const EXCEPTION_RECORD& record)
{
    return record.ExceptionCode == cppExceptionCode
           && record.NumberParameters >= cppExceptionParameterCount
           && record.ExceptionInformation[0] == cppExceptionMagic
           && record.ExceptionInformation[throwInfoParameter] != 0;
}

void readCppException(
    const EXCEPTION_RECORD& record,
    const std::function<void(std::string_view, std::string_view)>& recordMessage)
{
    auto thrownType = std::optional<ThrownType> {};
    auto stdException = std::optional<ThrownType> {};

    forEachCatchableType(record,
                         [&](const ThrownType& type)
                         {
                             if (!thrownType)
                                 thrownType = type;

                             if (type.decoratedName == stdExceptionTypeName)
                                 stdException = type;
                         });

    if (!thrownType)
        return;

    auto buffer = std::array<char, maxUndecoratedTypeLength> {};
    auto typeName = undecorateTypeName(thrownType->decoratedName, buffer);
    auto detail = std::string_view {};

    if (stdException)
    {
        auto object = record.ExceptionInformation[thrownObjectParameter];
        auto displacement =
            static_cast<std::uintptr_t>(stdException->memberDisplacement);
        const auto& error =
            *reinterpret_cast<const std::exception*>(object + displacement);
        detail = error.what();
    }

    recordMessage(typeName, detail);
}
} // namespace eacp::CrashReporting::Detail
