#include "Common.h"
#include "KillContentProcess.h"

using namespace nano;
using namespace eacp;

namespace
{
// Posts `ready` on every load, so the page coming back is a second message.
const std::string contentProcessPageHtml = R"HTML(<!doctype html><html><body><script>
  window.webkit.messageHandlers.ready.postMessage('ready');
</script></body></html>)HTML";

Graphics::WebView::Options makeContentProcessOptions()
{
    auto options = Graphics::WebView::Options {};
    options.schemes["testterm"] =
        [](std::string_view) -> std::optional<Graphics::ResourceResponse>
    {
        auto response = Graphics::ResourceResponse {};
        response.mimeType = "text/html; charset=utf-8";
        response.data.assign(contentProcessPageHtml.begin(),
                             contentProcessPageHtml.end());
        return response;
    };
    options.userDataFolderSuffix = "contentprocesstermination";
    return options;
}

struct ContentProcessFixture
{
    Graphics::WebView webView {makeContentProcessOptions()};
    Graphics::Window window {};
    int loads = 0;

    ContentProcessFixture()
    {
        window.setContentView(webView);
        webView.addScriptMessageHandler("ready",
                                        [this](const std::string&) { ++loads; });
    }

    void waitForLoads(int expectedLoads)
    {
        check(Threads::runEventLoopUntil([this, expectedLoads]
                                         { return loads >= expectedLoads; },
                                         firstNavigationTimeout));
    }
};
} // namespace

auto tPageReloadsAfterContentProcessDies =
    test("ContentProcessTermination/pageReloadsAfterContentProcessDies") = []
{
    auto fix = ContentProcessFixture {};
    fix.webView.loadURL("testterm://host/index.html");
    fix.waitForLoads(1);

    killContentProcess(fix.webView);

    fix.waitForLoads(2);
};

auto tHtmlPageComesBackAfterContentProcessDies =
    test("ContentProcessTermination/htmlPageComesBackAfterContentProcessDies") = []
{
    auto fix = ContentProcessFixture {};
    fix.webView.loadHTML(contentProcessPageHtml);
    fix.waitForLoads(1);

    killContentProcess(fix.webView);

    fix.waitForLoads(2);
};
