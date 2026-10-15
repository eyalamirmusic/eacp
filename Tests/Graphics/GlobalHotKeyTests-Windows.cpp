#include "Common.h"

// GlobalHotKey::isAvailable asks Windows itself, by registering the chord and
// releasing it again, so the honest test holds a chord for real and asks. The
// suite runs headless, where a GlobalHotKey registers nothing, so these turn
// that off for as long as one is alive.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
// Unlikely to be bound by anything on a test machine, and harmless if it is
// pressed while held: the callback does nothing.
constexpr auto heldChord =
    ModifierKeys {.shift = true, .control = true, .alt = true};
constexpr auto heldKey = KeyCode::F12;

struct NotHeadless
{
    NotHeadless()
        : previous(Apps::getAppEnvironment().headless)
    {
        Apps::getAppEnvironment().headless = false;
    }

    ~NotHeadless() { Apps::getAppEnvironment().headless = previous; }

    bool previous;
};
} // namespace

auto tHotKeyHeldChordIsTaken = test("GlobalHotKey/aHeldChordIsNotAvailable") = []
{
    check(GlobalHotKey::isAvailable(heldChord, heldKey),
          "nothing else on this machine holds the test chord");

    auto notHeadless = NotHeadless {};

    {
        auto hotKey = GlobalHotKey {heldChord, heldKey, [] {}};
        check(!GlobalHotKey::isAvailable(heldChord, heldKey),
              "a chord this process holds is taken");
    }

    check(GlobalHotKey::isAvailable(heldChord, heldKey),
          "and free again once the hot key is gone");
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
