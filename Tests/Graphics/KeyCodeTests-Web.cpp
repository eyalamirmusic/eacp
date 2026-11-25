#include "Common.h"

#include <eacp/Graphics/Graphics/Keyboard-Web.h>

#include <set>

// KeyCodeTests.cpp checks the framework side; this checks the web's table
// from KeyboardEvent.code, the text a key event carries and the polled state
// the events leave behind, none of which needs a page.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
// Every constant Keyboard.h defines except Unknown.
const uint16_t webAllKeyCodes[] = {
    KeyCode::A,
    KeyCode::S,
    KeyCode::D,
    KeyCode::F,
    KeyCode::H,
    KeyCode::G,
    KeyCode::Z,
    KeyCode::X,
    KeyCode::C,
    KeyCode::V,
    KeyCode::B,
    KeyCode::Q,
    KeyCode::W,
    KeyCode::E,
    KeyCode::R,
    KeyCode::Y,
    KeyCode::T,
    KeyCode::O,
    KeyCode::U,
    KeyCode::I,
    KeyCode::P,
    KeyCode::L,
    KeyCode::J,
    KeyCode::K,
    KeyCode::N,
    KeyCode::M,
    KeyCode::Num0,
    KeyCode::Num1,
    KeyCode::Num2,
    KeyCode::Num3,
    KeyCode::Num4,
    KeyCode::Num5,
    KeyCode::Num6,
    KeyCode::Num7,
    KeyCode::Num8,
    KeyCode::Num9,
    KeyCode::Space,
    KeyCode::Return,
    KeyCode::Tab,
    KeyCode::Delete,
    KeyCode::Escape,
    KeyCode::Back,
    KeyCode::LeftArrow,
    KeyCode::RightArrow,
    KeyCode::DownArrow,
    KeyCode::UpArrow,
    KeyCode::F1,
    KeyCode::F2,
    KeyCode::F3,
    KeyCode::F4,
    KeyCode::F5,
    KeyCode::F6,
    KeyCode::F7,
    KeyCode::F8,
    KeyCode::F9,
    KeyCode::F10,
    KeyCode::F11,
    KeyCode::F12,
    KeyCode::Minus,
    KeyCode::Equals,
    KeyCode::LeftBracket,
    KeyCode::RightBracket,
    KeyCode::Backslash,
    KeyCode::Semicolon,
    KeyCode::Quote,
    KeyCode::Comma,
    KeyCode::Period,
    KeyCode::Slash,
    KeyCode::Grave,
    KeyCode::Home,
    KeyCode::End,
    KeyCode::PageUp,
    KeyCode::PageDown,
    KeyCode::ForwardDelete,
    KeyCode::CapsLock,
    KeyCode::Shift,
    KeyCode::RightShift,
    KeyCode::Control,
    KeyCode::RightControl,
    KeyCode::Option,
    KeyCode::RightOption,
    KeyCode::Command,
    KeyCode::RightCommand,
    KeyCode::KeypadEnter,
    KeyCode::Keypad0,
    KeyCode::Keypad1,
    KeyCode::Keypad2,
    KeyCode::Keypad3,
    KeyCode::Keypad4,
    KeyCode::Keypad5,
    KeyCode::Keypad6,
    KeyCode::Keypad7,
    KeyCode::Keypad8,
    KeyCode::Keypad9,
    KeyCode::KeypadDecimal,
    KeyCode::KeypadPlus,
    KeyCode::KeypadMinus,
    KeyCode::KeypadMultiply,
    KeyCode::KeypadDivide,
    KeyCode::KeypadClear,
    KeyCode::KeypadEquals,
};

constexpr auto webShift = 1;
constexpr auto webControl = 2;
constexpr auto webMeta = 8;

KeyEvent webPress(const char* code, const char* key, int modifiers = 0)
{
    auto event = webKeyEvent(true, code, key, modifiers, false, 1000.0);
    webKeyboardEvent(event);
    return event;
}

KeyEvent webRelease(const char* code, const char* key, int modifiers = 0)
{
    auto event = webKeyEvent(false, code, key, modifiers, false, 1001.0);
    webKeyboardEvent(event);
    return event;
}
} // namespace

auto tWebEveryKeyCodeHasACode = test("KeyCodeWeb/everyKeyCodeMapsToADomCode") = []
{
    for (auto keyCode: webAllKeyCodes)
        check(!webCodeFromKeyCode(keyCode).empty());

    check(webCodeFromKeyCode(KeyCode::Unknown).empty());
};

auto tWebRoundTrip = test("KeyCodeWeb/domCodeRoundTripsBackToTheSameKeyCode") = []
{
    for (auto keyCode: webAllKeyCodes)
        check(webKeyCodeFromCode(webCodeFromKeyCode(keyCode)) == keyCode);
};

auto tWebCodesAreUnique = test("KeyCodeWeb/noTwoKeysShareADomCode") = []
{
    auto seen = std::set<std::string_view> {};

    for (auto keyCode: webAllKeyCodes)
        check(seen.insert(webCodeFromKeyCode(keyCode)).second);

    check(seen.size() == std::size(webAllKeyCodes));
};

auto tWebUnknownCodes = test("KeyCodeWeb/aCodeOutsideTheTableIsUnknown") = []
{
    check(webKeyCodeFromCode("MediaPlayPause") == KeyCode::Unknown);
    check(webKeyCodeFromCode("AudioVolumeUp") == KeyCode::Unknown);
    check(webKeyCodeFromCode("") == KeyCode::Unknown);
};

auto tWebCrossedNames = test("KeyCodeWeb/theNamesThatCrossAreMappedRight") = []
{
    check(webKeyCodeFromCode("Backspace") == KeyCode::Delete);
    check(webKeyCodeFromCode("Delete") == KeyCode::ForwardDelete);
    check(webKeyCodeFromCode("MetaLeft") == KeyCode::Command);
    check(webKeyCodeFromCode("AltRight") == KeyCode::RightOption);
    check(webKeyCodeFromCode("NumpadEnter") == KeyCode::KeypadEnter);
};

auto tWebModifierMask = test("KeyCodeWeb/modifierMaskUnpacks") = []
{
    auto modifiers = webModifiersFromMask(webShift | webMeta);

    check(modifiers.shift);
    check(!modifiers.control);
    check(!modifiers.alt);
    check(modifiers.command);
};

auto tWebPrintableText = test("KeyCodeWeb/aPrintableKeyTypesItsKey") = []
{
    auto event = webPress("KeyA", "a");
    webRelease("KeyA", "a");

    check(event.keyCode == KeyCode::A);
    check(event.type == KeyEventType::Down);
    check(event.characters == "a");
    check(event.charactersIgnoringModifiers == "a");
    check(same(event.timestamp, 1.0));
};

auto tWebShiftedText = test("KeyCodeWeb/shiftKeepsThePlainCharacterApart") = []
{
    webPress("KeyQ", "q");
    webRelease("KeyQ", "q");

    auto event = webPress("KeyQ", "Q", webShift);
    webRelease("KeyQ", "Q", webShift);

    check(event.characters == "Q");
    check(event.charactersIgnoringModifiers == "q");
    check(event.modifiers.shift);
};

auto tWebLayoutText = test("KeyCodeWeb/thePlainCharacterFollowsTheLayout") = []
{
    // AZERTY puts A where a US layout has Q.
    webPress("KeyQ", "a");
    webRelease("KeyQ", "a");

    check(Keyboard::keyCodeToCharacter(KeyCode::Q) == "a");
    check(Keyboard::keyCodeToCharacter(KeyCode::Num7) == "7");
    check(Keyboard::keyCodeToCharacter(KeyCode::Shift).empty());
};

auto tWebNamedKeys = test("KeyCodeWeb/namedKeysTypeTheirControlCharacters") = []
{
    check(webKeyEvent(true, "Enter", "Enter", 0, false, 0).characters == "\r");
    check(webKeyEvent(true, "Tab", "Tab", 0, false, 0).characters == "\t");
    check(webKeyEvent(true, "Backspace", "Backspace", 0, false, 0).characters
          == "\b");
    check(webKeyEvent(true, "Escape", "Escape", 0, false, 0).characters == "\x1b");
    check(
        webKeyEvent(true, "ArrowLeft", "ArrowLeft", 0, false, 0).characters.empty());
    check(webKeyEvent(true, "ShiftLeft", "Shift", 0, false, 0).characters.empty());
};

auto tWebNonAsciiText = test("KeyCodeWeb/oneCharacterOfAnyLengthIsText") = []
{
    auto event = webKeyEvent(true, "KeyE", "\xc3\xa9", 0, false, 0);

    check(event.characters == "\xc3\xa9");
};

auto tWebRepeatOnlyOnDown = test("KeyCodeWeb/onlyAPressRepeats") = []
{
    check(webKeyEvent(true, "KeyA", "a", 0, true, 0).isRepeat);
    check(!webKeyEvent(false, "KeyA", "a", 0, true, 0).isRepeat);
};

auto tWebPolledState = test("KeyCodeWeb/polledStateFollowsTheEvents") = []
{
    webKeyboardReset();

    webPress("ControlLeft", "Control", webControl);
    webPress("KeyS", "s", webControl);

    check(Keyboard::isKeyPressed(KeyCode::Control));
    check(Keyboard::isKeyPressed(KeyCode::S));
    check(Keyboard::isControlPressed());
    check(Keyboard::getPressedKeys().size() == 2);

    webRelease("KeyS", "s", webControl);

    check(!Keyboard::isKeyPressed(KeyCode::S));
    check(Keyboard::isKeyPressed(KeyCode::Control));

    webKeyboardReset();

    check(!Keyboard::isKeyPressed(KeyCode::Control));
    check(!Keyboard::isControlPressed());
    check(Keyboard::getPressedKeys().empty());
};
