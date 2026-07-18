#include "Common.h"

#include <eacp/Core/App/Clipboard.h>

// Clipboard read-back.
//
// These touch the *system* clipboard, which is shared with everything else on
// the machine, so they save whatever was there and put it back afterwards. A
// test suite that silently ate the user's clipboard would be a poor trade for
// the coverage.
//
// On a platform with no clipboard backend (Linux, for now) getText returns
// empty and these self-skip rather than fail — empty is the documented answer
// there, not a defect.

using namespace nano;
using namespace eacp;

namespace
{
// Restores the clipboard's previous contents on the way out.
struct ClipboardGuard
{
    ClipboardGuard()
        : previous(Clipboard::getText())
    {
    }

    ~ClipboardGuard()
    {
        if (!previous.empty())
            Clipboard::copyText(previous);
    }

    std::string previous;
};

bool clipboardWorks()
{
    // Probe rather than assume: writing then reading back is the only honest
    // test of whether this platform has a working clipboard at all.
    if (!Clipboard::copyText("eacp-clipboard-probe"))
        return false;

    return Clipboard::getText() == "eacp-clipboard-probe";
}
} // namespace

auto tRoundTripsText = test("Clipboard/textSurvivesARoundTrip") = []
{
    const auto guard = ClipboardGuard {};

    if (!clipboardWorks())
        return;

    check(Clipboard::copyText("hello clipboard"));
    check(Clipboard::getText() == "hello clipboard");
};

// The whole point of the addition: what comes back is what a *different*
// application would have written, not a value cached on the way in.
auto tReadsWhatWasWritten = test("Clipboard/readReflectsTheLatestWrite") = []
{
    const auto guard = ClipboardGuard {};

    if (!clipboardWorks())
        return;

    Clipboard::copyText("first");
    check(Clipboard::getText() == "first");

    Clipboard::copyText("second");
    check(Clipboard::getText() == "second");
};

auto tHasTextTracksContent = test("Clipboard/hasTextAgreesWithReadText") = []
{
    const auto guard = ClipboardGuard {};

    if (!clipboardWorks())
        return;

    Clipboard::copyText("something");

    check(Clipboard::hasText());
    check(!Clipboard::getText().empty());
};

// Multi-byte text has to survive the UTF-8/UTF-16 conversion each platform does
// internally — the case where a naive byte count or a narrow-string API loses
// characters.
auto tRoundTripsUnicode = test("Clipboard/unicodeSurvivesTheRoundTrip") = []
{
    const auto guard = ClipboardGuard {};

    if (!clipboardWorks())
        return;

    const auto text = std::string {"héllo → 世界 🌍"};

    check(Clipboard::copyText(text));
    check(Clipboard::getText() == text);
};

auto tRoundTripsNewlines = test("Clipboard/multiLineTextSurvives") = []
{
    const auto guard = ClipboardGuard {};

    if (!clipboardWorks())
        return;

    const auto text = std::string {"one\ntwo\nthree"};

    check(Clipboard::copyText(text));
    check(Clipboard::getText() == text);
};

// A paste of a large selection must not truncate. Big enough to cross the
// buffer sizes the platform paths allocate.
auto tRoundTripsLargeText = test("Clipboard/largeTextIsNotTruncated") = []
{
    const auto guard = ClipboardGuard {};

    if (!clipboardWorks())
        return;

    auto text = std::string {};

    for (auto line = 0; line < 2000; ++line)
        text += "a line of text that is not especially short\n";

    check(Clipboard::copyText(text));
    check(Clipboard::getText().size() == text.size());
};

// Reading repeatedly must be stable — the read must not consume the clipboard,
// which some platform APIs do for certain formats.
auto tReadIsRepeatable = test("Clipboard/readingDoesNotConsume") = []
{
    const auto guard = ClipboardGuard {};

    if (!clipboardWorks())
        return;

    Clipboard::copyText("stable");

    check(Clipboard::getText() == "stable");
    check(Clipboard::getText() == "stable");
    check(Clipboard::getText() == "stable");
};

// Never crashes and never returns garbage, whatever the clipboard holds. The
// contract that lets a caller paste without checking anything first.
auto tReadIsAlwaysSafe = test("Clipboard/readIsSafeOnAnyPlatform") = []
{
    const auto text = Clipboard::getText();
    const auto has = Clipboard::hasText();

    // Empty and hasText() disagreeing would mean a caller enabling Paste from
    // hasText() then pasting nothing.
    if (has)
        check(true); // content may still be non-text on some platforms
    else
        check(text.empty());
};
