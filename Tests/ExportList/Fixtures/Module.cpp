#include <cstddef>
#include <functional>
#include <string>

// A module linked against nothing but the import library made from the export
// list: every call below into ExportListFixture binds to the host executable.
namespace ExportListFixture
{
struct Key
{
    int value = 0;
};

int plainFunction(int value);
int archiveOnlyFunction();

template <typename T>
T twice(T value);

extern template int twice<int>(int);

inline int inlineCounter = 0;

__declspec(noinline) inline int inlineFunction(int value)
{
    return value + inlineCounter++;
}
} // namespace ExportListFixture

template <>
struct std::hash<ExportListFixture::Key>
{
    std::size_t operator()(const ExportListFixture::Key& key) const noexcept;
};

extern "C" __declspec(dllexport) int exportListModuleEntry(int value)
{
    using namespace ExportListFixture;

    auto text = std::to_string(value);
    auto hashed = std::hash<Key> {}(Key {value});

    return plainFunction(value) + archiveOnlyFunction() + twice(value)
           + inlineFunction(value) + static_cast<int>(text.size() + hashed);
}
