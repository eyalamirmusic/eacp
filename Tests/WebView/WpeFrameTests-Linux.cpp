#include "Common.h"

#include <eacp/WebView/WebView/GLibPump-Linux.h>
#include <eacp/WebView/WebView/WpeFrame-Linux.h>

#include <glib.h>

#include <cmath>

// The Linux backend's own pieces: the frames WPE hands over becoming an
// image, end to end through takeSnapshot and on their own, and GLib's main
// context running under eacp's loop.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace WpeFrameTests
{
const std::string redPageHtml = R"HTML(<!doctype html><html><head><style>
  html,body{margin:0;height:100%;background:rgb(255,0,0)}
</style></head><body><script>
  setTimeout(function () {
    window.webkit.messageHandlers.ready.postMessage('ready');
  }, 50);
</script></body></html>)HTML";

bool channelNear(float channel, int target)
{
    return std::abs((int) std::lround(channel * 255.f) - target) <= 8;
}

struct RedPage
{
    WebView webView {};
    Window window {};
    bool ready = false;

    RedPage()
    {
        window.setContentView(webView);
        webView.setBounds({0.f, 0.f, 64.f, 48.f});
        webView.addScriptMessageHandler(
            "ready", [this](const std::string&) { ready = true; });
        webView.loadHTML(redPageHtml);
        check(Threads::runEventLoopUntil([this] { return ready; },
                                         firstNavigationTimeout));
    }

    Image snapshot()
    {
        auto png = Bytes {};
        auto error = std::string {};
        auto answered = false;

        webView.takeSnapshot(
            [&](Bytes bytes, const std::string& message)
            {
                png = std::move(bytes);
                error = message;
                answered = true;
            });

        check(Threads::runEventLoopUntil([&] { return answered; },
                                         webViewResultTimeout));
        check(error.empty());

        return Image::decode(png);
    }
};
} // namespace WpeFrameTests

auto tRedPageSnapshotsRed = test("WpeFrame/redPageSnapshotsRed") = []
{
    auto page = WpeFrameTests::RedPage {};
    auto image = page.snapshot();

    check(image.isValid());
    check(image.width() == 64);
    check(image.height() == 48);

    auto centre = image.at(32, 24);
    check(WpeFrameTests::channelNear(centre.r, 255));
    check(WpeFrameTests::channelNear(centre.g, 0));
    check(WpeFrameTests::channelNear(centre.b, 0));
    check(WpeFrameTests::channelNear(centre.a, 255));
};

auto tPremultipliedBgraBecomesStraightRgba =
    test("WpeFrame/premultipliedBgraBecomesStraightRgba") = []
{
    const std::uint8_t bgra[] = {0, 0, 128, 128, 0, 0, 0, 0, 255, 0, 0, 255};
    auto image = detail::imageFromPremultipliedBgra(bgra, 3, 1, 12);

    check(image.isValid());

    const auto& rgba = image.pixels();
    check(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 128);
    check(rgba[4] == 0 && rgba[5] == 0 && rgba[6] == 0 && rgba[7] == 0);
    check(rgba[8] == 0 && rgba[9] == 0 && rgba[10] == 255 && rgba[11] == 255);
};

auto tGLibTimeoutAndIdleFire =
    test("GLibPump/timeoutAndIdleFireUnderTheEventLoop") = []
{
    check(detail::ensureGLibPump());

    auto timeoutFired = false;
    auto idleFired = false;

    auto setFlag = [](gpointer flag) -> gboolean
    {
        *static_cast<bool*>(flag) = true;
        return G_SOURCE_REMOVE;
    };

    g_timeout_add(20, setFlag, &timeoutFired);
    g_idle_add(setFlag, &idleFired);

    check(Threads::runEventLoopUntil([&] { return timeoutFired && idleFired; },
                                     Time::MS {2000}));
};
