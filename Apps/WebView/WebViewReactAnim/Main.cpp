#include "Types.h"

#include <eacp/WebView/WebView.h>

using namespace eacp;
using namespace Graphics;

struct MyApp
{
    MyApp()
    {
        // Binds clock.getCurrentTick to the bridge's command table and
        // subscribes a listener to clock.tick that re-emits each
        // published snapshot over the WebView wire.
        transport.getBridge().use(clock);

        setApplicationMenuBar(buildDefaultWebViewMenuBar());
        window.setContentView(webView);
    }

    // Declaration order matters: clock comes first so it's destructed
    // *last*, after the transport's bound listeners have torn down.
    // (EA::Broadcaster's destructor also nulls listener back-pointers
    // as a defence-in-depth, but the right ordering is the contract.)
    Api::Clock clock;
    WebView webView {embeddedOptions("ReactAnimApp")};
    WebViewBridge transport {webView};
    Window window;
    Threads::Timer timer {[&] { clock.update(); }, 120};
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
