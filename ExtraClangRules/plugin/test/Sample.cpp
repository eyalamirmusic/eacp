#include <functional>
#include <string>
#include <vector>

struct Widget
{
    std::function<void()> onClick;                      // expect: no default
    std::function<void()> onHover = {};                 // expect: null default
    std::function<int()> onCount = nullptr;             // expect: null default
    std::function<void()> onGood = [] {};               // ok
    std::function<std::string()> makeName = [] { return std::string {}; }; // ok
    int plain = 0;                                      // ok (not std::function)
};

using Callback = std::function<void()>;

struct Aliased
{
    Callback viaAlias; // expect: no default (alias resolves to std::function)
};

auto badReturn() // NOLINT(eacp-no-auto-function-return)
{
    return 42;
}

auto trailing() -> int // expect: auto return
{
    return 1;
}

int goodReturn()
{
    std::string name = std::string("hi"); // expect: use auto
    auto fine = std::string("ok");
    std::size_t n = name.size();     // expect: use auto (size_t == size_t)
    int converted = name.size();     // ok: conversion, auto would change type
    Widget* raw = new Widget();      // expect: raw new (+ use auto? types match)
    delete raw;                      // expect: raw delete
    // a narration comment            expect: body comment
    for (const std::string& s : std::vector<std::string> {})
        (void) s;
    return static_cast<int>(n);
}

struct Shape
{
    virtual ~Shape() = default;
    auto area() const { return 1.0; } // expect: auto return
    int width() const { return 2; }
};
