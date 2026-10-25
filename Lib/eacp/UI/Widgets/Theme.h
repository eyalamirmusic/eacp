#pragma once

#include "../Common.h"

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace eacp::UI
{
// What a theme can hold: a colour, a number (a corner radius, a line width, a
// size) or a face.
template <typename T>
concept ThemeValueType =
    std::same_as<T, Color> || std::same_as<T, float> || std::same_as<T, Font>;

using ThemeValue = std::variant<Color, float, Font>;

// FNV-1a over a key's name, at compile time for a key declared constexpr, so a
// lookup in a paint() hashes nothing.
constexpr std::uint64_t themeKeyId(std::string_view name)
{
    auto hash = 14695981039346656037ull;

    for (auto c: name)
    {
        hash ^= (std::uint8_t) c;
        hash *= 1099511628211ull;
    }

    return hash;
}

// A name and the type of value it names. Declared once, constexpr, by whoever
// draws with it -- eacp's stock widgets in ThemeKeys.h, a library or an app
// anywhere else -- and then used to set and to read, so a misspelt name is a
// compile error rather than a colour that silently never arrives. Names are
// dotted by owner, "comboBox.highlight", so two owners do not collide.
template <ThemeValueType T>
class ThemeKey
{
public:
    constexpr explicit ThemeKey(std::string_view nameToUse)
        : name(nameToUse)
        , id(themeKeyId(nameToUse))
    {
    }

    constexpr std::string_view getName() const { return name; }
    constexpr std::uint64_t getId() const { return id; }

private:
    std::string_view name;
    std::uint64_t id;
};

using ColourKey = ThemeKey<Color>;
using NumberKey = ThemeKey<float>;
using FontKey = ThemeKey<Font>;

class Theme;

// eacp's own values for the keys its stock widgets read (ThemeKeys.h): what
// every tree draws in until it is given a theme, and what any theme falls back
// to for a key it does not hold.
const Theme& defaultTheme();

// A look as data: keys to values, any number of them, of any owner's keys. A
// tree's widgets draw in their host's (ComponentHost::setTheme); a value a
// widget was given itself, such as setAccentColour, wins over it.
//
// Reading falls back in order: this theme, then defaultTheme(), then the value
// the caller passes. So a theme need only hold what it changes, and a key no
// one has set -- one of an app's own that eacp has never heard of -- reads as
// what its caller says it should.
class Theme
{
public:
    template <ThemeValueType T>
    void set(const ThemeKey<T>& key, std::type_identity_t<T> value)
    {
        setValue(key.getName(), key.getId(), std::move(value));
    }

    // This theme's own value, null when it holds none. No fallback: what a
    // theme says, rather than what drawing with it would use.
    template <ThemeValueType T>
    const T* find(const ThemeKey<T>& key) const
    {
        if (auto* value = findValue(key.getName(), key.getId()))
            return std::get_if<T>(value);

        return nullptr;
    }

    template <ThemeValueType T>
    bool contains(const ThemeKey<T>& key) const
    {
        return find(key) != nullptr;
    }

    // What drawing with this theme uses for the key: its own value, else
    // defaultTheme()'s, else the fallback.
    template <ThemeValueType T>
    T get(const ThemeKey<T>& key, const T& fallback) const
    {
        if (auto* own = find(key))
            return *own;

        if (const auto& base = defaultTheme(); &base != this)
            if (auto* stock = base.find(key))
                return *stock;

        return fallback;
    }

    template <ThemeValueType T>
    T get(const ThemeKey<T>& key) const
    {
        return get(key, T {});
    }

    int size() const { return (int) entries.size(); }
    bool isEmpty() const { return entries.empty(); }

private:
    // The name is kept beside the value and compared on every read, so two
    // names that happened to hash alike would read as unset rather than as each
    // other's.
    struct Entry
    {
        std::string name;
        ThemeValue value;
    };

    void setValue(std::string_view name, std::uint64_t id, ThemeValue value);
    const ThemeValue* findValue(std::string_view name, std::uint64_t id) const;

    std::unordered_map<std::uint64_t, Entry> entries;
};
} // namespace eacp::UI
