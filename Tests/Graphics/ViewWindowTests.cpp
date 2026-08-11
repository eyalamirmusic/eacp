#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

auto tViewHasNoWindowUntilAdopted = test("View/hasNoWindowUntilAdopted") = []
{
    auto view = View {};

    check(view.getWindow() == nullptr);

    auto window = Window {};
    window.setContentView(view);

    check(view.getWindow() == &window);
};

auto tSubviewFindsTheWindow = test("View/subviewFindsTheWindow") = []
{
    auto content = View {};
    auto child = View {};
    auto grandchild = View {};

    content.addSubview(child);
    child.addSubview(grandchild);

    check(grandchild.getWindow() == nullptr);

    auto window = Window {};
    window.setContentView(content);

    check(child.getWindow() == &window);
    check(grandchild.getWindow() == &window);

    child.removeFromParent();

    check(child.getWindow() == nullptr);
    check(grandchild.getWindow() == nullptr);
    check(content.getWindow() == &window);
};

auto tViewOutlivingItsWindowReportsNone = test("View/outlivingItsWindowIsSafe") = []
{
    auto view = View {};

    {
        auto window = Window {};
        window.setContentView(view);
        check(view.getWindow() == &window);
    }

    check(view.getWindow() == nullptr);
};

auto tAdoptingASecondViewReleasesTheFirst =
    test("View/adoptingASecondViewReleasesTheFirst") = []
{
    auto first = View {};
    auto second = View {};
    auto window = Window {};

    window.setContentView(first);
    check(first.getWindow() == &window);

    window.setContentView(second);

    check(second.getWindow() == &window);
    check(first.getWindow() == nullptr);
};
