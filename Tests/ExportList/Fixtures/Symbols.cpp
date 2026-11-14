#include <cstddef>
#include <functional>
#include <string>

// One of every kind of definition the export list sorts. ExportListTests.cpp
// names each by its decorated name.
namespace ExportListFixture
{
struct Key
{
    int value = 0;
};

int plainFunction(int value)
{
    return value * 3;
}

int writableGlobal = 7;

const char* greeting()
{
    return "a string literal the list never exports";
}

inline int inlineCounter = 0;

__declspec(noinline) inline int inlineFunction(int value)
{
    return value + inlineCounter++;
}

using IntFunction = int (*)(int);

IntFunction inlineFunctionAddress()
{
    return &inlineFunction;
}

template <typename T>
T twice(T value)
{
    return value + value;
}

template int twice<int>(int);

using ToString = std::string (*)(int);

ToString standardInlineFunction()
{
    return &std::to_string;
}

struct Shape
{
    virtual ~Shape();
    virtual int sides() const;
};

Shape::~Shape() = default;

int Shape::sides() const
{
    return 0;
}

Shape* makeShape()
{
    return new Shape;
}
} // namespace ExportListFixture

template <>
struct std::hash<ExportListFixture::Key>
{
    std::size_t operator()(const ExportListFixture::Key& key) const noexcept;
};

std::size_t std::hash<ExportListFixture::Key>::operator()(
    const ExportListFixture::Key& key) const noexcept
{
    return static_cast<std::size_t>(key.value);
}
