#include "Common.h"

#include <eacp/Graphics/Graphics/Keyboard-Linux.h>

#include <linux/input-event-codes.h>

#include <set>

// The Linux half of the KeyCode table: framework key codes against the kernel's
// evdev numbering, which is what wl_keyboard.key carries.
//
// KeyCodeTests.cpp next door checks the framework side - that no two names
// share a value. This checks the mapping, which is the part that can be wrong
// while the header is right: a transposed evdev number gives a key that works
// perfectly and reports itself as a different one, and the symptom is a
// shortcut firing on the wrong key on Linux only.
//
// Worth a suite of its own because there is no other coverage of it. The seat
// this table serves does not exist on a headless compositor, so
// WaylandWindowTests cannot press a key; everything below runs anywhere,
// including with no display at all.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
struct LinuxNamedKey
{
    const char* name;
    std::uint16_t code;
};

// Every constant Keyboard.h defines except Unknown, which is the answer for a
// key that is NOT in the table.
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

// Coverage. A KeyCode with no evdev key behind it is one an app can name in a
// shortcut and never see fire, which is worse than not having the constant.
auto tEveryKeyCodeHasAnEvdevKey =
    test("KeyCodeLinux/everyKeyCodeMapsToAnEvdevKey") = []
{
    for (const auto& key: linuxAllKeys)
        check(waylandEvdevFromKeyCode(key.code) != 0);

    check(waylandEvdevFromKeyCode(KeyCode::Unknown) == 0);
};

// The round trip, which is what actually catches a transposed number: a
// duplicate evdev value maps two names onto one key, and the second one comes
// back as the first.
auto tRoundTripIsExact =
    test("KeyCodeLinux/evdevRoundTripsBackToTheSameKeyCode") = []
{
    for (const auto& key: linuxAllKeys)
    {
        const auto evdev = waylandEvdevFromKeyCode(key.code);
        check(waylandKeyCodeFromEvdev(evdev) == key.code);
    }
};

auto tEvdevCodesAreUnique = test("KeyCodeLinux/noTwoKeysShareAnEvdevCode") = []
{
    auto seen = std::set<std::uint32_t> {};

    for (const auto& key: linuxAllKeys)
        check(seen.insert(waylandEvdevFromKeyCode(key.code)).second);

    check(seen.size() == std::size(linuxAllKeys));
};

// A key outside the table is Unknown rather than something plausible. Media
// keys are the common case: a keyboard sends them constantly and nothing in
// the framework names them.
auto tUnmappedKeysAreUnknown = test("KeyCodeLinux/aKeyOutsideTheTableIsUnknown") = []
{
    check(waylandKeyCodeFromEvdev(KEY_PLAYPAUSE) == KeyCode::Unknown);
    check(waylandKeyCodeFromEvdev(KEY_VOLUMEUP) == KeyCode::Unknown);
    check(waylandKeyCodeFromEvdev(0) == KeyCode::Unknown);
    check(waylandKeyCodeFromEvdev(0xFFFF) == KeyCode::Unknown);
};

// The spot checks worth writing out, because these are the entries most likely
// to be got wrong and least likely to be noticed.
//
// Delete is backspace and ForwardDelete is the other one - the framework names
// keys for what they do and evdev names them the other way round, so the two
// are crossed exactly here and nowhere else. The letters are the alphabet in
// none of the three orderings involved (QWERTY, alphabetical, macOS virtual
// key), so a table copied from the wrong column shows up here first.
auto tCrossedNamesAreRight =
    test("KeyCodeLinux/theNamesThatCrossAreMappedRight") = []
{
    check(waylandEvdevFromKeyCode(KeyCode::Delete) == KEY_BACKSPACE);
    check(waylandEvdevFromKeyCode(KeyCode::ForwardDelete) == KEY_DELETE);

    check(waylandEvdevFromKeyCode(KeyCode::A) == KEY_A);
    check(waylandEvdevFromKeyCode(KeyCode::Z) == KEY_Z);
    check(waylandEvdevFromKeyCode(KeyCode::Num0) == KEY_0);
    check(waylandEvdevFromKeyCode(KeyCode::Num1) == KEY_1);
    check(waylandEvdevFromKeyCode(KeyCode::Return) == KEY_ENTER);
    check(waylandEvdevFromKeyCode(KeyCode::KeypadEnter) == KEY_KPENTER);
    check(waylandEvdevFromKeyCode(KeyCode::Escape) == KEY_ESC);
    check(waylandEvdevFromKeyCode(KeyCode::Quote) == KEY_APOSTROPHE);
    check(waylandEvdevFromKeyCode(KeyCode::Period) == KEY_DOT);
    check(waylandEvdevFromKeyCode(KeyCode::Equals) == KEY_EQUAL);
    check(waylandEvdevFromKeyCode(KeyCode::LeftBracket) == KEY_LEFTBRACE);
    check(waylandEvdevFromKeyCode(KeyCode::RightBracket) == KEY_RIGHTBRACE);
};

// With no seat there is no keyboard state, and every query says so rather than
// inventing one. This is the state a headless compositor - and any machine with
// no compositor at all - leaves the process in, so it is also what the whole
// rest of the suite runs against.
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
