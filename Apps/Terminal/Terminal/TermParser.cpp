#include "TermParser.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace term
{
namespace
{
// DEC special graphics (ESC ( 0): the 0x60..0x7e range becomes line-drawing
// glyphs. Everything full-screen apps draw borders with on a non-UTF-8 path.
constexpr std::array<char32_t, 31> decGraphics = {
    U'◆', U'▒', U'␉', U'␌', U'␍', U'␊', U'°',
    U'±', U'␤', U'␋', U'┘', U'┐', U'┌', U'└',
    U'┼', U'⎺', U'⎻', U'─', U'⎼', U'⎽', U'├',
    U'┤', U'┴', U'┬', U'│', U'≤', U'≥', U'π',
    U'≠', U'£', U'·'};
} // namespace

TermParser::TermParser(TermScreen& screenToUse, const Theme& themeToUse)
    : screen(screenToUse)
    , theme(themeToUse)
{
    params.reserve(8);
}

void TermParser::feed(std::string_view bytes)
{
    for (const auto c: bytes)
        processByte((std::uint8_t) c);
}

void TermParser::startSequence(State next)
{
    state = next;
    params.clear();
    params.emplace_back();
    privateMarker = 0;
    intermediate = 0;
    oscBuffer.clear();
    stringEscPending = false;
}

void TermParser::processByte(std::uint8_t byte)
{
    switch (state)
    {
        case State::Ground:
        {
            if (byte == 0x1b)
            {
                utf8Remaining = 0;
                state = State::Escape;
                return;
            }

            if (byte < 0x20 || byte == 0x7f)
            {
                executeControl(byte);
                return;
            }

            if (utf8Remaining > 0)
            {
                if ((byte & 0xc0) == 0x80)
                {
                    utf8Codepoint = (utf8Codepoint << 6) | (byte & 0x3f);

                    if (--utf8Remaining == 0)
                        putCodepoint(utf8Codepoint);
                }
                else
                {
                    utf8Remaining = 0;
                    putCodepoint(U'�');
                    processByte(byte);
                }

                return;
            }

            if (byte < 0x80)
            {
                putCodepoint(byte);
            }
            else if ((byte & 0xe0) == 0xc0)
            {
                utf8Codepoint = byte & 0x1f;
                utf8Remaining = 1;
            }
            else if ((byte & 0xf0) == 0xe0)
            {
                utf8Codepoint = byte & 0x0f;
                utf8Remaining = 2;
            }
            else if ((byte & 0xf8) == 0xf0)
            {
                utf8Codepoint = byte & 0x07;
                utf8Remaining = 3;
            }
            else
            {
                putCodepoint(U'�');
            }

            return;
        }

        case State::Escape:
            dispatchEscape(byte);
            return;

        case State::Csi:
        {
            if (byte == 0x1b)
            {
                state = State::Escape;
                return;
            }

            if (byte == 0x18 || byte == 0x1a)
            {
                state = State::Ground;
                return;
            }

            if (byte < 0x20)
            {
                executeControl(byte);
                return;
            }

            if (byte >= '0' && byte <= '9')
            {
                auto& group = params.back();

                if (group.empty())
                    group.push_back(0);

                auto& value = group.back();
                value = std::min(value * 10 + (byte - '0'), 65535);
                return;
            }

            if (byte == ';')
            {
                params.emplace_back();
                return;
            }

            if (byte == ':')
            {
                auto& group = params.back();

                if (group.empty())
                    group.push_back(0);

                group.push_back(0);
                return;
            }

            if (byte >= 0x3c && byte <= 0x3f)
            {
                privateMarker = (char) byte;
                return;
            }

            if (byte >= 0x20 && byte <= 0x2f)
            {
                intermediate = (char) byte;
                return;
            }

            if (byte >= 0x40 && byte <= 0x7e)
            {
                dispatchCsi(byte);
                state = State::Ground;
            }

            return;
        }

        case State::Osc:
        {
            if (byte == 0x07)
            {
                dispatchOsc();
                state = State::Ground;
                return;
            }

            if (stringEscPending)
            {
                stringEscPending = false;
                dispatchOsc();

                if (byte == '\\')
                    state = State::Ground;
                else
                    dispatchEscape(byte);

                return;
            }

            if (byte == 0x1b)
            {
                stringEscPending = true;
                return;
            }

            if (oscBuffer.size() < 4096)
                oscBuffer.push_back((char) byte);

            return;
        }

        case State::StringSequence:
        {
            // DCS/SOS/PM/APC content is consumed and dropped.
            if (byte == 0x07)
            {
                state = State::Ground;
                return;
            }

            if (stringEscPending)
            {
                stringEscPending = false;
                state = byte == '\\' ? State::Ground : State::Escape;

                if (state == State::Escape)
                    dispatchEscape(byte);

                return;
            }

            if (byte == 0x1b)
                stringEscPending = true;

            return;
        }

        case State::Charset:
        {
            const auto special = byte == '0';

            if (charsetTarget >= 0 && charsetTarget < 2)
                charsetSpecial[charsetTarget] = special;

            state = State::Ground;
            return;
        }
    }
}

void TermParser::executeControl(std::uint8_t byte)
{
    switch (byte)
    {
        case 0x07:
            onBell();
            break;
        case 0x08:
            screen.backspace();
            break;
        case 0x09:
            screen.horizontalTab();
            break;
        case 0x0a:
        case 0x0b:
        case 0x0c:
            screen.linefeed();
            break;
        case 0x0d:
            screen.carriageReturn();
            break;
        case 0x0e:
            activeCharset = 1;
            break;
        case 0x0f:
            activeCharset = 0;
            break;
        default:
            break;
    }
}

void TermParser::dispatchEscape(std::uint8_t byte)
{
    state = State::Ground;

    switch (byte)
    {
        case '[':
            startSequence(State::Csi);
            break;
        case ']':
            startSequence(State::Osc);
            break;
        case 'P':
        case 'X':
        case '^':
        case '_':
            startSequence(State::StringSequence);
            break;
        case '(':
            charsetTarget = 0;
            state = State::Charset;
            break;
        case ')':
            charsetTarget = 1;
            state = State::Charset;
            break;
        case '7':
            screen.saveCursor();
            break;
        case '8':
            screen.restoreCursor();
            break;
        case 'D':
            screen.linefeed();
            break;
        case 'E':
            screen.carriageReturn();
            screen.linefeed();
            break;
        case 'H':
            screen.setTabStop();
            break;
        case 'M':
            screen.reverseIndex();
            break;
        case 'Z':
            respond("\033[?6c");
            break;
        case 'c':
            screen.fullReset();
            charsetSpecial[0] = charsetSpecial[1] = false;
            activeCharset = 0;
            cursorStyle = 0;
            break;
        case '=':
            screen.modes.appKeypad = true;
            break;
        case '>':
            screen.modes.appKeypad = false;
            break;
        default:
            break;
    }
}

int TermParser::param(std::size_t index, int fallback) const
{
    if (index >= params.size() || params[index].empty())
        return fallback;

    return params[index][0];
}

int TermParser::paramOrOne(std::size_t index) const
{
    return std::max(param(index, 1), 1);
}

void TermParser::dispatchCsi(std::uint8_t final)
{
    if (privateMarker == '?' && (final == 'h' || final == 'l'))
    {
        for (std::size_t i = 0; i < params.size(); ++i)
            setPrivateMode(param(i, -1), final == 'h');

        return;
    }

    if (privateMarker != 0 && final != 'c')
        return;

    switch (final)
    {
        case '@':
            screen.insertChars(paramOrOne(0));
            break;
        case 'A':
            screen.moveCursor(0, -paramOrOne(0));
            break;
        case 'B':
        case 'e':
            screen.moveCursor(0, paramOrOne(0));
            break;
        case 'C':
        case 'a':
            screen.moveCursor(paramOrOne(0), 0);
            break;
        case 'D':
            screen.moveCursor(-paramOrOne(0), 0);
            break;
        case 'E':
            screen.moveCursor(0, paramOrOne(0));
            screen.carriageReturn();
            break;
        case 'F':
            screen.moveCursor(0, -paramOrOne(0));
            screen.carriageReturn();
            break;
        case 'G':
        case '`':
            screen.setColumn(paramOrOne(0) - 1);
            break;
        case 'H':
        case 'f':
            screen.setCursor(paramOrOne(1) - 1, paramOrOne(0) - 1);
            break;
        case 'I':
            screen.horizontalTab(paramOrOne(0));
            break;
        case 'J':
            screen.eraseInDisplay(param(0, 0));
            break;
        case 'K':
            screen.eraseInLine(param(0, 0));
            break;
        case 'L':
            screen.insertLines(paramOrOne(0));
            break;
        case 'M':
            screen.deleteLines(paramOrOne(0));
            break;
        case 'P':
            screen.deleteChars(paramOrOne(0));
            break;
        case 'S':
            screen.scrollUp(paramOrOne(0));
            break;
        case 'T':
            screen.scrollDown(paramOrOne(0));
            break;
        case 'X':
            screen.eraseChars(paramOrOne(0));
            break;
        case 'Z':
            screen.backTab(paramOrOne(0));
            break;
        case 'b':
            screen.repeatLastChar(paramOrOne(0));
            break;
        case 'c':
            if (privateMarker == '>')
                respond("\033[>0;95;0c");
            else if (privateMarker == 0)
                respond("\033[?6c");
            break;
        case 'd':
            screen.setRow(paramOrOne(0) - 1);
            break;
        case 'g':
            screen.clearTabStops(param(0, 0));
            break;
        case 'h':
        case 'l':
            for (std::size_t i = 0; i < params.size(); ++i)
                setAnsiMode(param(i, -1), final == 'h');
            break;
        case 'm':
            dispatchSgr();
            break;
        case 'n':
        {
            const auto request = param(0, 0);

            if (request == 5)
            {
                respond("\033[0n");
            }
            else if (request == 6)
            {
                char report[32];
                std::snprintf(report,
                              sizeof(report),
                              "\033[%d;%dR",
                              screen.cursor.y + 1,
                              screen.cursor.x + 1);
                respond(report);
            }

            break;
        }
        case 'q':
            if (intermediate == ' ')
                cursorStyle = param(0, 0);
            break;
        case 'r':
            screen.setScrollRegion(param(0, 1) - 1,
                                   param(1, screen.rows()) - 1);
            break;
        case 's':
            screen.saveCursor();
            break;
        case 'u':
            screen.restoreCursor();
            break;
        default:
            break;
    }
}

void TermParser::setPrivateMode(int mode, bool on)
{
    auto& modes = screen.modes;

    switch (mode)
    {
        case 1:
            modes.appCursorKeys = on;
            break;
        case 6:
            modes.origin = on;
            screen.setCursor(0, 0);
            break;
        case 7:
            modes.autowrap = on;
            break;
        case 12:
            modes.cursorBlink = on;
            break;
        case 25:
            modes.showCursor = on;
            break;
        case 47:
        case 1047:
            screen.useAltScreen(on, false);
            break;
        case 1000:
            modes.mouseButtons = on;
            break;
        case 1002:
            modes.mouseDrag = on;
            break;
        case 1003:
            modes.mouseMotion = on;
            break;
        case 1006:
            modes.mouseSgr = on;
            break;
        case 1048:
            if (on)
                screen.saveCursor();
            else
                screen.restoreCursor();
            break;
        case 1049:
            screen.useAltScreen(on, true);
            break;
        case 2004:
            modes.bracketedPaste = on;
            break;
        default:
            break;
    }
}

void TermParser::setAnsiMode(int, bool)
{
    // IRM/KAM and friends are rare enough to ignore for now.
}

std::size_t
    TermParser::parseExtendedColor(std::size_t index, Rgb& color, bool& valid)
{
    valid = false;
    const auto& group = params[index];

    // Colon form carries the spec inside one group: 38:5:n or 38:2[:id]:r:g:b.
    if (group.size() >= 2)
    {
        if (group[1] == 5 && group.size() >= 3)
        {
            color = theme.indexed(group[2]);
            valid = true;
        }
        else if (group[1] == 2 && group.size() >= 5)
        {
            const auto offset = group.size() >= 6 ? 3 : 2;
            const auto clampByte = [](int v)
            { return (Rgb) std::clamp(v, 0, 255); };
            color = (clampByte(group[offset]) << 16)
                    | (clampByte(group[offset + 1]) << 8)
                    | clampByte(group[offset + 2]);
            valid = true;
        }

        return 0;
    }

    // Semicolon form spreads across groups: 38;5;n or 38;2;r;g;b.
    if (index + 1 >= params.size())
        return 0;

    const auto kind = param(index + 1, -1);

    if (kind == 5 && index + 2 < params.size())
    {
        color = theme.indexed(param(index + 2, 0));
        valid = true;
        return 2;
    }

    if (kind == 2 && index + 4 < params.size())
    {
        const auto clampByte = [&](std::size_t i)
        { return (Rgb) std::clamp(param(i, 0), 0, 255); };
        color = (clampByte(index + 2) << 16) | (clampByte(index + 3) << 8)
                | clampByte(index + 4);
        valid = true;
        return 4;
    }

    return 0;
}

void TermParser::dispatchSgr()
{
    auto& pen = screen.pen;

    for (std::size_t i = 0; i < params.size(); ++i)
    {
        const auto code = param(i, 0);

        switch (code)
        {
            case 0:
                pen.attrs = 0;
                pen.defaultFg = true;
                pen.defaultBg = true;
                pen.fg = theme.foreground;
                pen.bg = theme.background;
                break;
            case 1:
                pen.attrs |= Attr::Bold;
                break;
            case 2:
                pen.attrs |= Attr::Faint;
                break;
            case 3:
                pen.attrs |= Attr::Italic;
                break;
            case 4:
                if (params[i].size() >= 2 && params[i][1] == 0)
                    pen.attrs &= (std::uint16_t) ~Attr::Underline;
                else
                    pen.attrs |= Attr::Underline;
                break;
            case 5:
            case 6:
                pen.attrs |= Attr::Blink;
                break;
            case 7:
                pen.attrs |= Attr::Inverse;
                break;
            case 8:
                pen.attrs |= Attr::Hidden;
                break;
            case 9:
                pen.attrs |= Attr::Strike;
                break;
            case 21:
            case 22:
                pen.attrs &= (std::uint16_t) ~(Attr::Bold | Attr::Faint);
                break;
            case 23:
                pen.attrs &= (std::uint16_t) ~Attr::Italic;
                break;
            case 24:
                pen.attrs &= (std::uint16_t) ~Attr::Underline;
                break;
            case 25:
                pen.attrs &= (std::uint16_t) ~Attr::Blink;
                break;
            case 27:
                pen.attrs &= (std::uint16_t) ~Attr::Inverse;
                break;
            case 28:
                pen.attrs &= (std::uint16_t) ~Attr::Hidden;
                break;
            case 29:
                pen.attrs &= (std::uint16_t) ~Attr::Strike;
                break;
            case 38:
            {
                auto color = Rgb {};
                auto valid = false;
                i += parseExtendedColor(i, color, valid);

                if (valid)
                {
                    pen.fg = color;
                    pen.defaultFg = false;
                }

                break;
            }
            case 39:
                pen.defaultFg = true;
                pen.fg = theme.foreground;
                break;
            case 48:
            {
                auto color = Rgb {};
                auto valid = false;
                i += parseExtendedColor(i, color, valid);

                if (valid)
                {
                    pen.bg = color;
                    pen.defaultBg = false;
                }

                break;
            }
            case 49:
                pen.defaultBg = true;
                pen.bg = theme.background;
                break;
            default:
                if (code >= 30 && code <= 37)
                {
                    pen.fg = theme.ansi[(std::size_t) code - 30];
                    pen.defaultFg = false;
                }
                else if (code >= 40 && code <= 47)
                {
                    pen.bg = theme.ansi[(std::size_t) code - 40];
                    pen.defaultBg = false;
                }
                else if (code >= 90 && code <= 97)
                {
                    pen.fg = theme.ansi[(std::size_t) code - 90 + 8];
                    pen.defaultFg = false;
                }
                else if (code >= 100 && code <= 107)
                {
                    pen.bg = theme.ansi[(std::size_t) code - 100 + 8];
                    pen.defaultBg = false;
                }
                break;
        }
    }
}

void TermParser::dispatchOsc()
{
    const auto separator = oscBuffer.find(';');

    if (separator == std::string::npos)
        return;

    const auto code = oscBuffer.substr(0, separator);
    const auto payload = oscBuffer.substr(separator + 1);

    if (code == "0" || code == "2")
    {
        onTitleChanged(payload);
        return;
    }

    // Color queries: vim probes these to pick its default background.
    if ((code == "10" || code == "11") && payload == "?")
    {
        const auto rgb = code == "10" ? theme.foreground : theme.background;
        char reply[64];
        std::snprintf(reply,
                      sizeof(reply),
                      "\033]%s;rgb:%02x%02x/%02x%02x/%02x%02x\033\\",
                      code.c_str(),
                      (rgb >> 16) & 0xff,
                      (rgb >> 16) & 0xff,
                      (rgb >> 8) & 0xff,
                      (rgb >> 8) & 0xff,
                      rgb & 0xff,
                      rgb & 0xff);
        respond(reply);
    }
}

void TermParser::putCodepoint(char32_t cp)
{
    if (charsetSpecial[activeCharset] && cp >= 0x60 && cp <= 0x7e)
        cp = decGraphics[cp - 0x60];

    screen.putChar(cp);
}
} // namespace term
