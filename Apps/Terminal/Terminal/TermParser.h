#pragma once

#include "TermScreen.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace term
{
// Feeds raw PTY bytes through a VT500-style state machine into a TermScreen:
// UTF-8 decoding, C0 controls, ESC/CSI/OSC dispatch, SGR (16/256/true color),
// DEC private modes and the DEC special-graphics charset. Query sequences
// (DA, DSR, OSC color queries) answer through respond, which writes back to
// the PTY.
class TermParser
{
public:
    TermParser(TermScreen& screenToUse, const Theme& themeToUse);

    void feed(std::string_view bytes);

    std::function<void(std::string_view)> respond = [](std::string_view) {};
    std::function<void(const std::string&)> onTitleChanged =
        [](const std::string&) {};
    std::function<void()> onBell = [] {};

    // OSC 7: the shell reporting its working directory (file:// URL).
    std::function<void(const std::string&)> onCwdChanged = [](const std::string&) {};

    // OSC 9 (or OSC 777;notify;title;body): a program inside the terminal
    // requesting a desktop notification.
    std::function<void(const std::string&)> onNotify = [](const std::string&) {};

    // DECSCUSR shape: 0/1 blinking block, 2 block, 3/4 underline, 5/6 bar.
    int cursorShape() const { return cursorStyle; }

private:
    enum class State
    {
        Ground,
        Escape,
        Csi,
        Osc,
        StringSequence,
        Charset
    };

    using ParamGroup = std::vector<int>;

    void processByte(std::uint8_t byte);
    void executeControl(std::uint8_t byte);
    void startSequence(State next);
    void dispatchEscape(std::uint8_t byte);
    void dispatchCsi(std::uint8_t final);
    void dispatchSgr();
    void dispatchOsc();
    void setPrivateMode(int mode, bool on);
    void setAnsiMode(int mode, bool on);
    void putCodepoint(char32_t cp);

    int param(std::size_t index, int fallback) const;
    int paramOrOne(std::size_t index) const;

    // Consumes an extended color spec (38/48) starting at group `index`,
    // returning how many extra groups were used.
    std::size_t parseExtendedColor(std::size_t index, Rgb& color, bool& valid);

    TermScreen& screen;
    const Theme& theme;

    State state = State::Ground;

    char32_t utf8Codepoint = 0;
    int utf8Remaining = 0;

    std::vector<ParamGroup> params;
    char privateMarker = 0;
    char intermediate = 0;

    std::string oscBuffer;
    bool stringEscPending = false;

    int charsetTarget = 0;
    bool charsetSpecial[2] = {false, false};
    int activeCharset = 0;

    int cursorStyle = 0;
};
} // namespace term
