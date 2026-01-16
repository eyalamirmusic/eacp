#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace eacp::Graphics
{

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
    static bool isKeyPressed(uint16_t keyCode);

    static bool isShiftPressed();
    static bool isControlPressed();
    static bool isAltPressed();
    static bool isCommandPressed();

    static ModifierKeys getModifiers();

    static std::vector<Key> getPressedKeys();

    static std::string keyCodeToCharacter(uint16_t keyCode);
};

} // namespace eacp::Graphics
