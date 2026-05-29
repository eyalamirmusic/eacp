#include "App.h"

#include <eacp/WebView/Test/TestApp.h>

#include <NanoTest/NanoTest.h>

using namespace std::chrono_literals;
using namespace eacp::WebView::Test;

using nano::check;

namespace
{

constexpr auto inputSelector = R"([data-testid="todo-input"])";
constexpr auto addSelector = R"([data-testid="todo-add"])";
constexpr auto itemSelector = R"([data-testid="todo-item"])";
constexpr auto textSelector = R"([data-testid="todo-text"])";
constexpr auto toggleSelector = R"([data-testid="todo-toggle"])";
constexpr auto removeSelector = R"([data-testid="todo-remove"])";
constexpr auto remainingSelector = R"([data-testid="todo-remaining"])";

std::string lastItemDescendant(const std::string& child)
{
    return std::string {itemSelector} + ":last-child " + child;
}

std::string firstItemDescendant(const std::string& child)
{
    return std::string {itemSelector} + ":first-child " + child;
}

TestApp<MyApp>& testApp()
{
    return createTestApp<MyApp>(inputSelector);
}

MyApp& app()
{
    return testApp().app();
}

AppDriver& driver()
{
    return testApp().driver();
}

Graphics::WebView& webView()
{
    return app().webView;
}

Async<std::string> callJS(const std::string& expression)
{
    return webView().callJS(expression);
}

} // namespace

auto tSeedsThreeTodos = test("WebViewTodo/seedsThreeTodosOnStartup") = []
{ check(driver().count(itemSelector) == 3); };

auto tAddsNewTodo = test("WebViewTodo/addsNewTodoViaForm") = []
{
    auto before = driver().count(itemSelector);

    driver().fill(inputSelector, "Buy milk");
    driver().click(addSelector);

    check(driver().count(itemSelector) == before + 1);
    check(driver().text(lastItemDescendant(textSelector)) == "Buy milk");
};

auto tToggleFlipsCompletion =
    test("WebViewTodo/toggleFlipsCompletionAndUpdatesFooter") = []
{
    auto before = std::stoi(driver().text(remainingSelector));

    driver().click(firstItemDescendant(toggleSelector));

    auto remaining = std::stoi(driver().text(remainingSelector));
    check(remaining == before - 1);
};

auto tRemovingTodo = test("WebViewTodo/removingTodoDecrementsCount") = []
{
    auto before = driver().count(itemSelector);

    driver().click(firstItemDescendant(removeSelector));

    check(driver().count(itemSelector) == before - 1);
};

auto tDomainRpcsReachable =
    test("WebViewTodo/domainRpcsReachableThroughSameBridge") = []
{
    // The bridge is shared with WebViewBridge, so the production
    // commands the React app calls (addTodo / getTodos) are also
    // reachable from the harness — handy for setting up state
    // without going through the UI.
    auto before = driver().invoke<TodoState>("getTodos");

    auto req = AddTodoRequest {};
    req.text = "Direct add via bridge";
    driver().invoke("addTodo", Miro::toJSON(req));

    auto after = driver().invoke<TodoState>("getTodos");

    check(after.items.size() == before.items.size() + 1);
    check(after.items[after.items.size() - 1].text == "Direct add via bridge");
};

auto tCallJsResolvesWithResult = test("WebViewTodo/callJsResolvesWithResult") = []
{
    auto result = callJS("1 + 2").waitFor(2s);
    check(result == "3");
};

auto tCallJsRejectsOnError = test("WebViewTodo/callJsRejectsOnJsException") = []
{
    auto threw = false;
    try
    {
        callJS("throw new Error('boom')").waitFor(2s);
    }
    catch (const AsyncError& e)
    {
        threw = true;
        // evaluateJavaScript surfaces NSError::localizedDescription, which
        // for WKWebView JS exceptions is a generic phrase rather than the
        // thrown message itself. We just verify that a non-empty error
        // text reached us.
        check(!std::string {e.what()}.empty());
    }
    check(threw);
};

auto tCallJsChainsViaCoroutine =
    test("WebViewTodo/callJsChainsViaCoroutine") = []() -> Async<>
{
    auto sum = co_await callJS("1 + 2");
    auto wrapped = co_await callJS("'val:' + (" + sum + ")");
    check(wrapped == "val:3");
};
