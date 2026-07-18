#include "Common.h"

#include <set>

// The KeyCode table.
//
// Mostly a uniqueness and coverage check, which sounds trivial until you
// consider how these are written: they are raw platform virtual key codes typed
// in from a reference table. A transposed digit gives two names the same value,
// and the symptom is one shortcut mysteriously firing another's action — a bug
// that is very hard to trace back to a header.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
struct NamedKey
{
    const char* name;
    std::uint16_t code;
};

// Every key the table defines, so the checks below cannot silently skip one
// that was added later.
const NamedKey allKeys[] = {
    {"A", KeyCode::A},           {"S", KeyCode::S},
    {"D", KeyCode::D},           {"F", KeyCode::F},
    {"H", KeyCode::H},           {"G", KeyCode::G},
    {"Z", KeyCode::Z},           {"X", KeyCode::X},
    {"C", KeyCode::C},           {"V", KeyCode::V},
    {"B", KeyCode::B},           {"Q", KeyCode::Q},
    {"W", KeyCode::W},           {"E", KeyCode::E},
    {"R", KeyCode::R},           {"Y", KeyCode::Y},
    {"T", KeyCode::T},           {"O", KeyCode::O},
    {"U", KeyCode::U},           {"I", KeyCode::I},
    {"P", KeyCode::P},           {"L", KeyCode::L},
    {"J", KeyCode::J},           {"K", KeyCode::K},
    {"N", KeyCode::N},           {"M", KeyCode::M},

    {"Num0", KeyCode::Num0},     {"Num1", KeyCode::Num1},
    {"Num2", KeyCode::Num2},     {"Num3", KeyCode::Num3},
    {"Num4", KeyCode::Num4},     {"Num5", KeyCode::Num5},
    {"Num6", KeyCode::Num6},     {"Num7", KeyCode::Num7},
    {"Num8", KeyCode::Num8},     {"Num9", KeyCode::Num9},

    {"Space", KeyCode::Space},   {"Return", KeyCode::Return},
    {"Tab", KeyCode::Tab},       {"Delete", KeyCode::Delete},
    {"Escape", KeyCode::Escape},

    {"LeftArrow", KeyCode::LeftArrow},
    {"RightArrow", KeyCode::RightArrow},
    {"DownArrow", KeyCode::DownArrow},
    {"UpArrow", KeyCode::UpArrow},

    {"F1", KeyCode::F1},         {"F2", KeyCode::F2},
    {"F3", KeyCode::F3},         {"F4", KeyCode::F4},
    {"F5", KeyCode::F5},         {"F6", KeyCode::F6},
    {"F7", KeyCode::F7},         {"F8", KeyCode::F8},
    {"F9", KeyCode::F9},         {"F10", KeyCode::F10},
    {"F11", KeyCode::F11},       {"F12", KeyCode::F12},

    {"Minus", KeyCode::Minus},   {"Equals", KeyCode::Equals},
    {"LeftBracket", KeyCode::LeftBracket},
    {"RightBracket", KeyCode::RightBracket},
    {"Backslash", KeyCode::Backslash},
    {"Semicolon", KeyCode::Semicolon},
    {"Quote", KeyCode::Quote},   {"Comma", KeyCode::Comma},
    {"Period", KeyCode::Period}, {"Slash", KeyCode::Slash},
    {"Grave", KeyCode::Grave},

    {"Home", KeyCode::Home},     {"End", KeyCode::End},
    {"PageUp", KeyCode::PageUp}, {"PageDown", KeyCode::PageDown},
    {"ForwardDelete", KeyCode::ForwardDelete},
    {"CapsLock", KeyCode::CapsLock},

    {"KeypadEnter", KeyCode::KeypadEnter},
    {"Keypad0", KeyCode::Keypad0},
    {"Keypad1", KeyCode::Keypad1},
    {"Keypad2", KeyCode::Keypad2},
    {"Keypad3", KeyCode::Keypad3},
    {"Keypad4", KeyCode::Keypad4},
    {"Keypad5", KeyCode::Keypad5},
    {"Keypad6", KeyCode::Keypad6},
    {"Keypad7", KeyCode::Keypad7},
    {"Keypad8", KeyCode::Keypad8},
    {"Keypad9", KeyCode::Keypad9},
    {"KeypadDecimal", KeyCode::KeypadDecimal},
    {"KeypadPlus", KeyCode::KeypadPlus},
    {"KeypadMinus", KeyCode::KeypadMinus},
    {"KeypadMultiply", KeyCode::KeypadMultiply},
    {"KeypadDivide", KeyCode::KeypadDivide},
    {"KeypadClear", KeyCode::KeypadClear},
    {"KeypadEquals", KeyCode::KeypadEquals},
};
} // namespace

// No two names may share a value. This is the check that catches a mistyped
// constant, whose symptom otherwise is one shortcut firing another's action.
auto tCodesAreUnique = test("KeyCode/everyKeyHasItsOwnCode") = []
{
    auto seen = std::set<std::uint16_t> {};

    for (const auto& key: allKeys)
    {
        const auto inserted = seen.insert(key.code).second;

        if (!inserted)
            check(false); // duplicate value — see the name in allKeys
    }

    check(seen.size() == std::size(allKeys));
};

// Unknown must not collide with a real key, or an unmapped platform code would
// be indistinguishable from whatever it collided with.
auto tUnknownIsDistinct = test("KeyCode/unknownCollidesWithNothing") = []
{
    for (const auto& key: allKeys)
        check(key.code != KeyCode::Unknown);
};

// The keys an editor cannot work without. Their absence is what drives
// downstream apps to hand-define raw platform codes, which then only work on
// the platform they were taken from.
auto tEditorKeysExist = test("KeyCode/navigationKeysAreDefined") = []
{
    check(KeyCode::Home != KeyCode::Unknown);
    check(KeyCode::End != KeyCode::Unknown);
    check(KeyCode::PageUp != KeyCode::Unknown);
    check(KeyCode::PageDown != KeyCode::Unknown);
    check(KeyCode::ForwardDelete != KeyCode::Unknown);

    // Backspace and forward delete are genuinely different keys, and an editor
    // binds them to opposite operations.
    check(KeyCode::Delete != KeyCode::ForwardDelete);
};

auto tPunctuationKeysExist = test("KeyCode/punctuationKeysAreDefined") = []
{
    // Cmd+[ / Cmd+] and Cmd+/ are standard editor bindings that cannot be
    // expressed without these.
    check(KeyCode::LeftBracket != KeyCode::RightBracket);
    check(KeyCode::Slash != KeyCode::Backslash);
    check(KeyCode::Minus != KeyCode::Equals);
    check(KeyCode::Quote != KeyCode::Grave);
};

// The keypad is a separate set of keys from the number row, so an app can bind
// them apart.
auto tKeypadIsDistinctFromNumberRow = test("KeyCode/keypadDiffersFromTheNumberRow") = []
{
    const std::uint16_t row[] = {KeyCode::Num0,
                                 KeyCode::Num1,
                                 KeyCode::Num2,
                                 KeyCode::Num3,
                                 KeyCode::Num4,
                                 KeyCode::Num5,
                                 KeyCode::Num6,
                                 KeyCode::Num7,
                                 KeyCode::Num8,
                                 KeyCode::Num9};

    const std::uint16_t keypad[] = {KeyCode::Keypad0,
                                    KeyCode::Keypad1,
                                    KeyCode::Keypad2,
                                    KeyCode::Keypad3,
                                    KeyCode::Keypad4,
                                    KeyCode::Keypad5,
                                    KeyCode::Keypad6,
                                    KeyCode::Keypad7,
                                    KeyCode::Keypad8,
                                    KeyCode::Keypad9};

    for (const auto number: row)
        for (const auto pad: keypad)
            check(number != pad);

    check(KeyCode::Return != KeyCode::KeypadEnter);
};

auto tKeyEventDefaults = test("KeyCode/keyEventDefaultsAreInert") = []
{
    const auto event = KeyEvent {};

    check(event.characters.empty());
    check(event.charactersIgnoringModifiers.empty());
    check(event.type == KeyEventType::Down);
    check(!event.isRepeat);
    check(!event.modifiers.shift);
    check(!event.modifiers.control);
    check(!event.modifiers.alt);
    check(!event.modifiers.command);
};
