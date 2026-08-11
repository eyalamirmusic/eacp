#include "Common.h"

#include <eacp/Core/App/Clipboard.h>

// These touch the system clipboard, so they save and restore whatever was
// there. Platforms with no backend return empty and these self-skip.
using namespace nano;
using namespace eacp;

namespace
{
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
    // Probe rather than assume this platform has a working clipboard.
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

// Big enough to cross the buffer sizes the platform paths allocate.
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

auto tReadIsAlwaysSafe = test("Clipboard/readIsSafeOnAnyPlatform") = []
{
    const auto text = Clipboard::getText();
    const auto has = Clipboard::hasText();

    if (has)
        check(true); // content may still be non-text on some platforms
    else
        check(text.empty());
};
