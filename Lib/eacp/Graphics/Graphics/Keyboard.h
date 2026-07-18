#pragma once

#include "../Common.h"

namespace eacp::Graphics
{

class Window;

namespace KeyCode
{
constexpr uint16_t A = 0x00;
constexpr uint16_t S = 0x01;
constexpr uint16_t D = 0x02;
constexpr uint16_t F = 0x03;
constexpr uint16_t H = 0x04;
constexpr uint16_t G = 0x05;
constexpr uint16_t Z = 0x06;
constexpr uint16_t X = 0x07;
constexpr uint16_t C = 0x08;
constexpr uint16_t V = 0x09;
constexpr uint16_t B = 0x0B;
constexpr uint16_t Q = 0x0C;
constexpr uint16_t W = 0x0D;
constexpr uint16_t E = 0x0E;
constexpr uint16_t R = 0x0F;
constexpr uint16_t Y = 0x10;
constexpr uint16_t T = 0x11;
constexpr uint16_t O = 0x1F;
constexpr uint16_t U = 0x20;
constexpr uint16_t I = 0x22;
constexpr uint16_t P = 0x23;
constexpr uint16_t L = 0x25;
constexpr uint16_t J = 0x26;
constexpr uint16_t K = 0x28;
constexpr uint16_t N = 0x2D;
constexpr uint16_t M = 0x2E;

constexpr uint16_t Num0 = 0x1D;
constexpr uint16_t Num1 = 0x12;
constexpr uint16_t Num2 = 0x13;
constexpr uint16_t Num3 = 0x14;
constexpr uint16_t Num4 = 0x15;
constexpr uint16_t Num5 = 0x17;
constexpr uint16_t Num6 = 0x16;
constexpr uint16_t Num7 = 0x1A;
constexpr uint16_t Num8 = 0x1C;
constexpr uint16_t Num9 = 0x19;

constexpr uint16_t Space = 0x31;
constexpr uint16_t Return = 0x24;
constexpr uint16_t Tab = 0x30;
constexpr uint16_t Delete = 0x33;
constexpr uint16_t Escape = 0x35;

constexpr uint16_t LeftArrow = 0x7B;
constexpr uint16_t RightArrow = 0x7C;
constexpr uint16_t DownArrow = 0x7D;
constexpr uint16_t UpArrow = 0x7E;

constexpr uint16_t F1 = 0x7A;
constexpr uint16_t F2 = 0x78;
constexpr uint16_t F3 = 0x63;
constexpr uint16_t F4 = 0x76;
constexpr uint16_t F5 = 0x60;
constexpr uint16_t F6 = 0x61;
constexpr uint16_t F7 = 0x62;
constexpr uint16_t F8 = 0x64;
constexpr uint16_t F9 = 0x65;
constexpr uint16_t F10 = 0x6D;
constexpr uint16_t F11 = 0x67;
constexpr uint16_t F12 = 0x6F;

// Punctuation, named for the *unshifted* key. Shift arrives separately in
// ModifierKeys, so Quote is the same key whether it produced ' or ".
//
// These matter for shortcuts, where the character a key produced is the wrong
// thing to match on: Cmd+[ and Cmd+] should navigate on any layout, but which
// character that key emits depends on the layout.
constexpr uint16_t Minus = 0x1B;
constexpr uint16_t Equals = 0x18;
constexpr uint16_t LeftBracket = 0x21;
constexpr uint16_t RightBracket = 0x1E;
constexpr uint16_t Backslash = 0x2A;
constexpr uint16_t Semicolon = 0x29;
constexpr uint16_t Quote = 0x27;
constexpr uint16_t Comma = 0x2B;
constexpr uint16_t Period = 0x2F;
constexpr uint16_t Slash = 0x2C;
constexpr uint16_t Grave = 0x32;

// Navigation. An editor needs every one of these, and their absence is why
// downstream apps end up hand-defining raw platform virtual key codes — which
// then only work on the platform they were lifted from.
constexpr uint16_t Home = 0x73;
constexpr uint16_t End = 0x77;
constexpr uint16_t PageUp = 0x74;
constexpr uint16_t PageDown = 0x79;

// Forward delete, as against Delete above, which is backspace. The names here
// follow what the key does rather than what the platform calls it.
constexpr uint16_t ForwardDelete = 0x75;

constexpr uint16_t CapsLock = 0x39;

// Keypad, distinct from the number row: a numeric keypad reports its own codes,
// and an app that folds them together cannot bind them separately.
constexpr uint16_t KeypadEnter = 0x4C;
constexpr uint16_t Keypad0 = 0x52;
constexpr uint16_t Keypad1 = 0x53;
constexpr uint16_t Keypad2 = 0x54;
constexpr uint16_t Keypad3 = 0x55;
constexpr uint16_t Keypad4 = 0x56;
constexpr uint16_t Keypad5 = 0x57;
constexpr uint16_t Keypad6 = 0x58;
constexpr uint16_t Keypad7 = 0x59;
constexpr uint16_t Keypad8 = 0x5B;
constexpr uint16_t Keypad9 = 0x5C;
constexpr uint16_t KeypadDecimal = 0x41;
constexpr uint16_t KeypadPlus = 0x45;
constexpr uint16_t KeypadMinus = 0x4E;
constexpr uint16_t KeypadMultiply = 0x43;
constexpr uint16_t KeypadDivide = 0x4B;
constexpr uint16_t KeypadClear = 0x47;
constexpr uint16_t KeypadEquals = 0x51;

// A platform key with no framework mapping. Backends that translate native
// codes (Windows) report it for keys outside the table above.
constexpr uint16_t Unknown = 0xFFFF;
} // namespace KeyCode

struct ModifierKeys
{
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool command = false;
};

enum class KeyEventType
{
    Down,
    Up
};

struct KeyEvent
{
    std::string characters;
    std::string charactersIgnoringModifiers;
    uint16_t keyCode = 0;
    KeyEventType type = KeyEventType::Down;
    ModifierKeys modifiers;
    bool isRepeat = false;
    double timestamp = 0.0;
};

struct Key
{
    uint16_t keyCode = 0;
    std::string character;
};

struct Keyboard
{
    // Window-scoped keyboard state. Preferred over the global variants below
    // when a window reference is available.
    static bool isKeyPressed(const Window& window, uint16_t keyCode);
    static bool isShiftPressed(const Window& window);
    static bool isControlPressed(const Window& window);
    static bool isAltPressed(const Window& window);
    static bool isCommandPressed(const Window& window);
    static ModifierKeys getModifiers(const Window& window);

    // Global keyboard state, for when no window reference is available.
    static bool isKeyPressed(uint16_t keyCode);
    static bool isShiftPressed();
    static bool isControlPressed();
    static bool isAltPressed();
    static bool isCommandPressed();
    static ModifierKeys getModifiers();

    static Vector<Key> getPressedKeys();

    static std::string keyCodeToCharacter(uint16_t keyCode);
};

} // namespace eacp::Graphics
