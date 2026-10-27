#include "Faults.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

#if defined(_WIN32)
#include <eacp/Core/Utils/WinInclude.h>
#include <intrin.h>
#endif

// Each fault has to survive the optimiser exactly as written.
#if defined(_MSC_VER)
#pragma optimize("", off)
#endif

namespace SelfDestruct
{
namespace
{
constexpr auto smallBlockSize = std::size_t {16};
constexpr auto stackFrameBytes = std::size_t {4096};

#if defined(_MSC_VER)
#define SELF_DESTRUCT_NO_INLINE __declspec(noinline)
#else
#define SELF_DESTRUCT_NO_INLINE [[gnu::noinline]]
#endif

SELF_DESTRUCT_NO_INLINE void dereferenceNullPointer()
{
    auto* volatile target = static_cast<int*>(nullptr);
    *target = 1;
}

volatile auto shouldKeepRecursing = true;

SELF_DESTRUCT_NO_INLINE int recurseForever(int depth)
{
    auto frame = std::array<volatile char, stackFrameBytes> {};
    frame[0] = static_cast<char>(depth);

    if (!shouldKeepRecursing)
        return frame[0];

    return recurseForever(depth + 1) + frame[0];
}

void overflowStack()
{
    recurseForever(0);
}

struct PureVirtualBase
{
    PureVirtualBase() { callWork(); }
    virtual ~PureVirtualBase() = default;

    SELF_DESTRUCT_NO_INLINE void callWork() { work(); }

    virtual void work() = 0;
};

struct PureVirtualDerived final : PureVirtualBase
{
    void work() override {}
};

void callPureVirtual()
{
    auto object = std::make_unique<PureVirtualDerived>();
    object->work();
}

void throwUncaught()
{
    throw std::runtime_error {"SelfDestruct threw this on purpose"};
}

#if defined(_WIN32)
// Only a fresh heap's second HeapFree is STATUS_HEAP_CORRUPTION every time.
constexpr auto heapCorruptionDescription = "Frees a heap block twice";

void corruptHeap()
{
    auto* heap = ::HeapCreate(0, 0, 0);
    auto* volatile block = ::HeapAlloc(heap, 0, smallBlockSize);
    ::HeapFree(heap, 0, block);
    ::HeapFree(heap, 0, block);
}
#else
constexpr auto overrunSize = std::size_t {4096};
constexpr auto overrunByte = 0xEA;
constexpr auto heapCorruptionDescription =
    "Writes past a heap block, then frees it twice";

// The overrun alone may land in slack; the second free is what every
// allocator checks.
void corruptHeap()
{
    auto* volatile block = static_cast<unsigned char*>(std::malloc(smallBlockSize));
    std::memset(block, overrunByte, overrunSize);
    std::free(block);
    std::free(block);
}
#endif

#if defined(_WIN32)
constexpr auto fatalAppExitFastFailCode = 7;

void failFast()
{
    __fastfail(fatalAppExitFastFailCode);
}
#endif

eacp::Vector<Fault> makeFaults()
{
    auto faults = eacp::Vector<Fault> {
        {"nullptr",
         "Null pointer",
         "Writes through a null pointer",
         dereferenceNullPointer},
        {"stackoverflow",
         "Stack overflow",
         "Recurses until the stack runs out",
         overflowStack},
        {"abort", "abort()", "Calls std::abort", [] { std::abort(); }},
        {"purevirtual",
         "Pure virtual call",
         "Calls a pure virtual from a constructor",
         callPureVirtual},
        {"terminate",
         "Uncaught exception",
         "Throws a std::runtime_error nobody catches",
         throwUncaught},
        {"heapcorruption",
         "Heap corruption",
         heapCorruptionDescription,
         corruptHeap},
    };

#if defined(_WIN32)
    faults.add({"fastfail",
                "__fastfail",
                "Fails fast, past every handler but WER",
                failFast});
#endif

    return faults;
}
} // namespace

const eacp::Vector<Fault>& getFaults()
{
    static const auto faults = makeFaults();
    return faults;
}

std::optional<std::reference_wrapper<const Fault>> findFault(std::string_view name)
{
    for (const auto& fault: getFaults())
    {
        if (fault.name == name)
            return fault;
    }

    return std::nullopt;
}
} // namespace SelfDestruct
