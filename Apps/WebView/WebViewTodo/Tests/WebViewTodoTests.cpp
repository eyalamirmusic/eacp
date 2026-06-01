#include "App.h"

#include <eacp/WebView/Test/TestApp.h>

#include <NanoTest/NanoTest.h>

#include <string>
#include <vector>

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
constexpr auto listSelector = R"([data-testid="todo-list"])";

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

// The tests below inspect the DOM the "traditional" way: capture an
// element subtree as a DomNode and walk it in C++ (tag/attr/class/
// children/find) instead of issuing one round-trip per assertion.

auto tInspectsItemStructure = test("WebViewTodo/inspectsItemStructureAsDomNode") = []
{
    auto item = driver().query(itemSelector);

    check(item.tag() == "li");
    check(item.hasClass("item"));
    check(item.attr("data-testid") == "todo-item");
    check(item.attr("data-todo-id") == "1");

    check(item.find(textSelector).text() == "Try editing me (double-click)");
    check(item.find(toggleSelector).tag() == "input");
};

auto tQueryAllReturnsEveryItem =
    test("WebViewTodo/queryAllReturnsEveryItemInOrder") = []
{
    auto items = driver().queryAll(itemSelector);
    check(items.size() == 3);

    auto texts = std::vector<std::string> {};
    for (auto& item: items)
        texts.push_back(item.find(textSelector).text());

    check(texts[0] == "Try editing me (double-click)");
    check(texts[1] == "Toggle a checkbox");
    check(texts[2] == "Add a new todo above");
};

auto tCompletedStateReflectedInDom =
    test("WebViewTodo/completedStateReflectedInClassAndCheckbox") = []
{
    auto items = driver().queryAll(itemSelector);

    check(!items[0].hasClass("done"));
    check(!items[0].find(toggleSelector).checked);

    // The third seed todo starts completed.
    check(items[2].hasClass("done"));
    check(items[2].find(toggleSelector).checked);
};

auto tTogglingUpdatesReQueriedNode =
    test("WebViewTodo/togglingUpdatesReQueriedNode") = []
{
    auto before = driver().query(itemSelector);
    check(!before.hasClass("done"));
    check(!before.find(toggleSelector).checked);

    driver().click(firstItemDescendant(toggleSelector));

    // DomNode is a snapshot, so re-query to see the toggled state.
    auto after = driver().query(itemSelector);
    check(after.hasClass("done"));
    check(after.find(toggleSelector).checked);
};

auto tListSubtreeExposesChildren =
    test("WebViewTodo/listSubtreeExposesChildrenAndAttributes") = []
{
    auto list = driver().query(listSelector);

    check(list.tag() == "ul");
    check(list.hasClass("list"));
    check(list.children.size() == 3);

    check(list.findAll(toggleSelector).size() == 3);

    auto removeButtons = list.findAll(removeSelector);
    check(removeButtons.size() == 3);
    check(removeButtons[0].attr("aria-label") == "Remove");
};

auto tInputAttributesReadable =
    test("WebViewTodo/inputAttributesReadableFromDomNode") = []
{
    auto input = driver().query(inputSelector);

    check(input.tag() == "input");
    check(input.attr("type") == "text");
    check(input.attr("placeholder") == "What needs to be done?");
    check(input.hasAttr("data-testid"));
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

// The reverse direction: the page registered `getRenderedTodos` via
// window.eacp.expose(...) (see web/src/main.tsx), and C++ calls it
// through the shared WebViewBridge. Unlike callJS — which evaluates a
// snippet and can't await — bridge.call awaits the page function's
// Promise and deserializes the resolved value into a typed struct.
namespace
{
struct RenderedTodos
{
    int count = 0;
    std::vector<std::string> texts;

    MIRO_REFLECT(count, texts)
};

Graphics::WebViewBridge& transport()
{
    return app().transport;
}
} // namespace

auto tCallsExposedAsyncPageFunction =
    test("WebViewTodo/callsExposedAsyncPageFunctionFromCpp") = []
{
    auto rendered = transport().call<RenderedTodos>("getRenderedTodos").waitFor(5s);

    check(rendered.count == 3);
    check(rendered.texts.size() == 3);
    check(rendered.texts[0] == "Try editing me (double-click)");
};

auto tExposedPageFunctionReflectsUiUpdates =
    test("WebViewTodo/exposedPageFunctionReflectsUiUpdatesFromCpp") = []
{
    driver().fill(inputSelector, "Call me from C++");
    driver().click(addSelector);

    auto rendered = transport().call<RenderedTodos>("getRenderedTodos").waitFor(5s);

    check(rendered.count == 4);
    check(rendered.texts.back() == "Call me from C++");
};

auto tCallsExposedPageFunctionViaCoroutine =
    test("WebViewTodo/callsExposedPageFunctionViaCoroutine") = []() -> Async<>
{
    auto rendered = co_await transport().call<RenderedTodos>("getRenderedTodos");
    check(rendered.count == 3);
};
