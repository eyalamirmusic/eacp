#include "Common.h"

#include <eacp/WebView/WebView/WebViewDetail.h>

// detail::withSnapshotDeadline is what makes WebView::takeSnapshot's callback
// unconditional. Both native snapshot calls can go quiet — WKWebView drops the
// completion handler of a snapshot taken against a hidden, throttled web
// process — and everything downstream (captureAsyncContent, and the promise
// renderToImageAsync hands back) settles only when the callback runs, so an
// unanswered snapshot would hang a caller's waitFor rather than fail it.
//
// Tested on the wrapper rather than through a real WebView because there is no
// way to ask a healthy backend to drop a completion handler: here "the platform
// never answered" is simply never calling the wrapped callback. The deadline is
// passed explicitly so these run in milliseconds instead of the real seconds.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto testDeadline = eacp::Time::MS {50};

// Long enough to carry a run of the event loop well past testDeadline — several
// ticks of the timer behind it — so a deadline that fired when it should not
// have has been seen by the time the test looks.
constexpr auto beyondDeadline = eacp::Time::MS {500};

// A bound rather than a sleep: a deadline that never fires at all fails the
// test instead of hanging it.
constexpr auto deadlineWaitLimit = eacp::Time::MS {5000};

struct Recorder
{
    int calls = 0;
    Bytes bytes;
    std::string error;

    WebView::SnapshotCallback callback()
    {
        return [this](Bytes pngBytes, const std::string& errorToUse)
        {
            ++calls;
            bytes = std::move(pngBytes);
            error = errorToUse;
        };
    }
};

Bytes somePng()
{
    return Bytes {1, 2, 3};
}
} // namespace

// The platform never answers: the callback still runs, reporting the failure
// rather than leaving the caller waiting on it forever.
auto tDeadlineAnswersUnansweredSnapshot =
    test("SnapshotDeadline/firesWhenPlatformNeverAnswers") = []
{
    auto recorder = Recorder {};
    auto guarded = detail::withSnapshotDeadline(recorder.callback(), testDeadline);

    check(Threads::runEventLoopUntil([&] { return recorder.calls > 0; },
                                     deadlineWaitLimit));
    check(recorder.calls == 1);
    check(recorder.bytes.size() == 0);
    check(!recorder.error.empty());
};

// The platform answers in time: its bytes are what the caller gets, and the
// deadline that fires afterwards must not deliver a second, empty answer over
// the top of them.
auto tPlatformAnswerWins = test("SnapshotDeadline/platformAnswerSuppressesIt") = []
{
    auto recorder = Recorder {};
    auto guarded = detail::withSnapshotDeadline(recorder.callback(), testDeadline);

    guarded(somePng(), "");

    check(recorder.calls == 1);
    check(recorder.bytes.size() == 3);
    check(recorder.error.empty());

    // Outlive the deadline, then confirm it stayed silent.
    Threads::runEventLoopFor(beyondDeadline);

    check(recorder.calls == 1);
    check(recorder.bytes.size() == 3);
};

// A platform that answers twice — or answers just as the deadline lands — still
// reaches the caller exactly once.
auto tAnswersOnlyOnce = test("SnapshotDeadline/deliversExactlyOneAnswer") = []
{
    auto recorder = Recorder {};
    auto guarded = detail::withSnapshotDeadline(recorder.callback(), testDeadline);

    guarded(somePng(), "");
    guarded({}, "late failure");

    Threads::runEventLoopFor(beyondDeadline);

    check(recorder.calls == 1);
    check(recorder.error.empty());
};
