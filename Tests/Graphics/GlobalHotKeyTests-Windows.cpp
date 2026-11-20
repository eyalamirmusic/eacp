#include "Common.h"

#include <windows.h>

// isAvailable asks Windows by registering the chord, so the test holds one for real.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
// Unlikely to be bound by anything on a test machine.
constexpr auto heldChord =
    ModifierKeys {.shift = true, .control = true, .alt = true};
constexpr auto heldKey = KeyCode::F12;

constexpr auto heldChordWin32Modifiers = UINT {MOD_SHIFT | MOD_CONTROL | MOD_ALT};
constexpr auto heldKeyWin32 = UINT {VK_F12};
constexpr auto heldChordId = 0xBFFE;

struct ChordHeldThroughWin32
{
    ChordHeldThroughWin32()
        : isHeld(RegisterHotKey(
                     nullptr, heldChordId, heldChordWin32Modifiers, heldKeyWin32)
                 != FALSE)
    {
    }

    ~ChordHeldThroughWin32()
    {
        if (isHeld)
            UnregisterHotKey(nullptr, heldChordId);
    }

    bool isHeld;
};
} // namespace

auto tHotKeyHeldChordIsTaken = test("GlobalHotKey/aHeldChordIsNotAvailable") = []
{
    check(GlobalHotKey::isAvailable(heldChord, heldKey),
          "nothing else on this machine holds the test chord");

    {
        auto held = ChordHeldThroughWin32 {};
        check(held.isHeld, "the test could hold its chord");
        check(!GlobalHotKey::isAvailable(heldChord, heldKey),
              "a chord this process holds is taken");
    }

    check(GlobalHotKey::isAvailable(heldChord, heldKey),
          "and free again once it is released");
};

auto tHotKeyProbeLeavesChordFree =
    test("GlobalHotKey/askingDoesNotHoldTheChord") = []
{
    check(GlobalHotKey::isAvailable(heldChord, heldKey));
    check(GlobalHotKey::isAvailable(heldChord, heldKey),
          "the probe released what it registered");
};

auto tHotKeyUnmappedKey =
    test("GlobalHotKey/aKeyWindowsCannotNameIsNotAvailable") = []
{ check(!GlobalHotKey::isAvailable(heldChord, KeyCode::Unknown)); };
