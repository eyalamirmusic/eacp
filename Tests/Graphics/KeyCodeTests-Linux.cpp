#include "Common.h"

#include <eacp/Graphics/Graphics/Keyboard-Linux.h>

#include <linux/input-event-codes.h>

#include <set>

// KeyCodeTests.cpp checks the framework side; this checks the evdev mapping,
// and needs no compositor to do it.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
struct LinuxNamedKey
{
    const char* name;
    std::uint16_t code;
};

// Every constant Keyboard.h defines except Unknown.
const LinuxNamedKey linuxAllKeys[] = {
    {"A", KeyCode::A},
    {"S", KeyCode::S},
    {"D", KeyCode::D},
    {"F", KeyCode::F},
    {"H", KeyCode::H},
    {"G", KeyCode::G},
    {"Z", KeyCode::Z},
    {"X", KeyCode::X},
    {"C", KeyCode::C},
    {"V", KeyCode::V},
    {"B", KeyCode::B},
    {"Q", KeyCode::Q},
    {"W", KeyCode::W},
    {"E", KeyCode::E},
    {"R", KeyCode::R},
    {"Y", KeyCode::Y},
    {"T", KeyCode::T},
    {"O", KeyCode::O},
    {"U", KeyCode::U},
    {"I", KeyCode::I},
    {"P", KeyCode::P},
    {"L", KeyCode::L},
    {"J", KeyCode::J},
    {"K", KeyCode::K},
    {"N", KeyCode::N},
    {"M", KeyCode::M},

    {"Num0", KeyCode::Num0},
    {"Num1", KeyCode::Num1},
    {"Num2", KeyCode::Num2},
    {"Num3", KeyCode::Num3},
    {"Num4", KeyCode::Num4},
    {"Num5", KeyCode::Num5},
    {"Num6", KeyCode::Num6},
    {"Num7", KeyCode::Num7},
    {"Num8", KeyCode::Num8},
    {"Num9", KeyCode::Num9},

    {"Space", KeyCode::Space},
    {"Return", KeyCode::Return},
    {"Tab", KeyCode::Tab},
    {"Delete", KeyCode::Delete},
    {"Escape", KeyCode::Escape},

    {"LeftArrow", KeyCode::LeftArrow},
    {"RightArrow", KeyCode::RightArrow},
    {"DownArrow", KeyCode::DownArrow},
    {"UpArrow", KeyCode::UpArrow},

    {"F1", KeyCode::F1},
    {"F2", KeyCode::F2},
    {"F3", KeyCode::F3},
    {"F4", KeyCode::F4},
    {"F5", KeyCode::F5},
    {"F6", KeyCode::F6},
    {"F7", KeyCode::F7},
    {"F8", KeyCode::F8},
    {"F9", KeyCode::F9},
    {"F10", KeyCode::F10},
    {"F11", KeyCode::F11},
    {"F12", KeyCode::F12},

    {"Minus", KeyCode::Minus},
    {"Equals", KeyCode::Equals},
    {"LeftBracket", KeyCode::LeftBracket},
    {"RightBracket", KeyCode::RightBracket},
    {"Backslash", KeyCode::Backslash},
    {"Semicolon", KeyCode::Semicolon},
    {"Quote", KeyCode::Quote},
    {"Comma", KeyCode::Comma},
    {"Period", KeyCode::Period},
    {"Slash", KeyCode::Slash},
    {"Grave", KeyCode::Grave},

    {"Home", KeyCode::Home},
    {"End", KeyCode::End},
    {"PageUp", KeyCode::PageUp},
    {"PageDown", KeyCode::PageDown},
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

auto tEveryKeyCodeHasAnEvdevKey =
    test("KeyCodeLinux/everyKeyCodeMapsToAnEvdevKey") = []
{
    for (const auto& key: linuxAllKeys)
        check(linuxEvdevFromKeyCode(key.code) != 0);

    check(linuxEvdevFromKeyCode(KeyCode::Unknown) == 0);
};

auto tRoundTripIsExact =
    test("KeyCodeLinux/evdevRoundTripsBackToTheSameKeyCode") = []
{
    for (const auto& key: linuxAllKeys)
    {
        const auto evdev = linuxEvdevFromKeyCode(key.code);
        check(linuxKeyCodeFromEvdev(evdev) == key.code);
    }
};

auto tEvdevCodesAreUnique = test("KeyCodeLinux/noTwoKeysShareAnEvdevCode") = []
{
    auto seen = std::set<std::uint32_t> {};

    for (const auto& key: linuxAllKeys)
        check(seen.insert(linuxEvdevFromKeyCode(key.code)).second);

    check(seen.size() == std::size(linuxAllKeys));
};

auto tUnmappedKeysAreUnknown = test("KeyCodeLinux/aKeyOutsideTheTableIsUnknown") = []
{
    check(linuxKeyCodeFromEvdev(KEY_PLAYPAUSE) == KeyCode::Unknown);
    check(linuxKeyCodeFromEvdev(KEY_VOLUMEUP) == KeyCode::Unknown);
    check(linuxKeyCodeFromEvdev(0) == KeyCode::Unknown);
    check(linuxKeyCodeFromEvdev(0xFFFF) == KeyCode::Unknown);
};

// Delete is backspace and ForwardDelete is the other one: the framework names
// keys for what they do and evdev names them the other way round.
auto tCrossedNamesAreRight =
    test("KeyCodeLinux/theNamesThatCrossAreMappedRight") = []
{
    check(linuxEvdevFromKeyCode(KeyCode::Delete) == KEY_BACKSPACE);
    check(linuxEvdevFromKeyCode(KeyCode::ForwardDelete) == KEY_DELETE);

    check(linuxEvdevFromKeyCode(KeyCode::A) == KEY_A);
    check(linuxEvdevFromKeyCode(KeyCode::Z) == KEY_Z);
    check(linuxEvdevFromKeyCode(KeyCode::Num0) == KEY_0);
    check(linuxEvdevFromKeyCode(KeyCode::Num1) == KEY_1);
    check(linuxEvdevFromKeyCode(KeyCode::Return) == KEY_ENTER);
    check(linuxEvdevFromKeyCode(KeyCode::KeypadEnter) == KEY_KPENTER);
    check(linuxEvdevFromKeyCode(KeyCode::Escape) == KEY_ESC);
    check(linuxEvdevFromKeyCode(KeyCode::Quote) == KEY_APOSTROPHE);
    check(linuxEvdevFromKeyCode(KeyCode::Period) == KEY_DOT);
    check(linuxEvdevFromKeyCode(KeyCode::Equals) == KEY_EQUAL);
    check(linuxEvdevFromKeyCode(KeyCode::LeftBracket) == KEY_LEFTBRACE);
    check(linuxEvdevFromKeyCode(KeyCode::RightBracket) == KEY_RIGHTBRACE);
};

auto tPolledStateIsEmptyWithoutASeat =
    test("KeyCodeLinux/polledStateIsEmptyWithoutASeat") = []
{
    check(!Keyboard::isKeyPressed(KeyCode::A));
    check(!Keyboard::isShiftPressed());
    check(!Keyboard::isControlPressed());
    check(!Keyboard::isAltPressed());
    check(!Keyboard::isCommandPressed());
    check(Keyboard::getPressedKeys().empty());
};
