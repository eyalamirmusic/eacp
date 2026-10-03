#pragma once

#include "../Graphics/Keyboard.h"

namespace eacp::Graphics
{

// USB HID keyboard usages (usage page 0x07), which is what a GCKeyCode is, to
// the framework's KeyCodes. KeyCode::Unknown for a usage outside the table.
constexpr uint16_t keyCodeFromHidUsage(uint32_t usage)
{
    struct HidMapping
    {
        uint32_t usage;
        uint16_t keyCode;
    };

    constexpr HidMapping mappings[] = {
        {0x04, KeyCode::A},
        {0x05, KeyCode::B},
        {0x06, KeyCode::C},
        {0x07, KeyCode::D},
        {0x08, KeyCode::E},
        {0x09, KeyCode::F},
        {0x0A, KeyCode::G},
        {0x0B, KeyCode::H},
        {0x0C, KeyCode::I},
        {0x0D, KeyCode::J},
        {0x0E, KeyCode::K},
        {0x0F, KeyCode::L},
        {0x10, KeyCode::M},
        {0x11, KeyCode::N},
        {0x12, KeyCode::O},
        {0x13, KeyCode::P},
        {0x14, KeyCode::Q},
        {0x15, KeyCode::R},
        {0x16, KeyCode::S},
        {0x17, KeyCode::T},
        {0x18, KeyCode::U},
        {0x19, KeyCode::V},
        {0x1A, KeyCode::W},
        {0x1B, KeyCode::X},
        {0x1C, KeyCode::Y},
        {0x1D, KeyCode::Z},

        {0x1E, KeyCode::Num1},
        {0x1F, KeyCode::Num2},
        {0x20, KeyCode::Num3},
        {0x21, KeyCode::Num4},
        {0x22, KeyCode::Num5},
        {0x23, KeyCode::Num6},
        {0x24, KeyCode::Num7},
        {0x25, KeyCode::Num8},
        {0x26, KeyCode::Num9},
        {0x27, KeyCode::Num0},

        {0x28, KeyCode::Return},
        {0x29, KeyCode::Escape},
        {0x2A, KeyCode::Delete},
        {0x2B, KeyCode::Tab},
        {0x2C, KeyCode::Space},
        {0x2D, KeyCode::Minus},
        {0x2E, KeyCode::Equals},
        {0x2F, KeyCode::LeftBracket},
        {0x30, KeyCode::RightBracket},
        {0x31, KeyCode::Backslash},
        {0x33, KeyCode::Semicolon},
        {0x34, KeyCode::Quote},
        {0x35, KeyCode::Grave},
        {0x36, KeyCode::Comma},
        {0x37, KeyCode::Period},
        {0x38, KeyCode::Slash},
        {0x39, KeyCode::CapsLock},

        {0x3A, KeyCode::F1},
        {0x3B, KeyCode::F2},
        {0x3C, KeyCode::F3},
        {0x3D, KeyCode::F4},
        {0x3E, KeyCode::F5},
        {0x3F, KeyCode::F6},
        {0x40, KeyCode::F7},
        {0x41, KeyCode::F8},
        {0x42, KeyCode::F9},
        {0x43, KeyCode::F10},
        {0x44, KeyCode::F11},
        {0x45, KeyCode::F12},

        {0x4A, KeyCode::Home},
        {0x4B, KeyCode::PageUp},
        {0x4C, KeyCode::ForwardDelete},
        {0x4D, KeyCode::End},
        {0x4E, KeyCode::PageDown},
        {0x4F, KeyCode::RightArrow},
        {0x50, KeyCode::LeftArrow},
        {0x51, KeyCode::DownArrow},
        {0x52, KeyCode::UpArrow},

        {0x53, KeyCode::KeypadClear},
        {0x54, KeyCode::KeypadDivide},
        {0x55, KeyCode::KeypadMultiply},
        {0x56, KeyCode::KeypadMinus},
        {0x57, KeyCode::KeypadPlus},
        {0x58, KeyCode::KeypadEnter},
        {0x59, KeyCode::Keypad1},
        {0x5A, KeyCode::Keypad2},
        {0x5B, KeyCode::Keypad3},
        {0x5C, KeyCode::Keypad4},
        {0x5D, KeyCode::Keypad5},
        {0x5E, KeyCode::Keypad6},
        {0x5F, KeyCode::Keypad7},
        {0x60, KeyCode::Keypad8},
        {0x61, KeyCode::Keypad9},
        {0x62, KeyCode::Keypad0},
        {0x63, KeyCode::KeypadDecimal},
        {0x67, KeyCode::KeypadEquals},

        {0xE0, KeyCode::Control},
        {0xE1, KeyCode::Shift},
        {0xE2, KeyCode::Option},
        {0xE3, KeyCode::Command},
        {0xE4, KeyCode::RightControl},
        {0xE5, KeyCode::RightShift},
        {0xE6, KeyCode::RightOption},
        {0xE7, KeyCode::RightCommand},
    };

    for (const auto& mapping: mappings)
        if (mapping.usage == usage)
            return mapping.keyCode;

    return KeyCode::Unknown;
}

} // namespace eacp::Graphics
