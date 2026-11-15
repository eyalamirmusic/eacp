#include "Common.h"

using namespace nano;
using namespace eacp::Graphics;

namespace OptionalViewTests
{
struct MarkedView final : View
{
    explicit MarkedView(bool& destroyedToUse)
        : destroyed(destroyedToUse)
    {
    }

    ~MarkedView() override { destroyed = true; }

    bool& destroyed;
};

eacp::OwningPointer<View> markedView(bool& destroyed)
{
    return eacp::OwningPointer<View> {new MarkedView {destroyed}};
}

auto tSetAddsAndSizes = test("OptionalView/setAddsAndSizesTheContent") = []
{
    auto holder = OptionalView<> {};
    holder.setBounds({0.f, 0.f, 200.f, 100.f});

    check(!holder.hasContent());
    check(!holder);

    auto destroyed = false;
    holder.set(markedView(destroyed));

    check(holder.hasContent());
    check(static_cast<bool>(holder));
    check(holder.getSubviews().size() == 1);
    check(holder.getSubviews()[0] == holder.get());
    check(holder->getParent() == &holder);
    check(same((*holder).getBounds(), {0.f, 0.f, 200.f, 100.f}));
};

auto tReplace = test("OptionalView/replacingDestroysTheOldContent") = []
{
    auto holder = OptionalView<> {};
    auto firstDestroyed = false;
    auto secondDestroyed = false;

    holder.set(markedView(firstDestroyed));
    holder.set(markedView(secondDestroyed));

    check(firstDestroyed);
    check(!secondDestroyed);
    check(holder.getSubviews().size() == 1);
    check(holder.getSubviews()[0] == holder.get());
};

// The platform calls resized() from its own layout pass, which a view outside
// any window never gets, so the case calls it as that pass would.
auto tResize = test("OptionalView/theContentFollowsAResize") = []
{
    auto holder = OptionalView<> {};
    auto destroyed = false;
    holder.set(markedView(destroyed));

    holder.setBounds({10.f, 20.f, 300.f, 150.f});
    holder.resized();

    check(same(holder->getBounds(), {0.f, 0.f, 300.f, 150.f}));
};

auto tClear = test("OptionalView/clearRemovesTheContent") = []
{
    auto holder = OptionalView<> {};
    auto destroyed = false;
    holder.set(markedView(destroyed));

    holder.clear();

    check(destroyed);
    check(!holder.hasContent());
    check(holder.get() == nullptr);
    check(holder.getSubviews().empty());
};
auto tSetNothing = test("OptionalView/settingNothingClears") = []
{
    auto holder = OptionalView<> {};
    auto destroyed = false;
    holder.set(markedView(destroyed));

    holder.set({});

    check(destroyed);
    check(!holder.hasContent());
    check(holder.getSubviews().empty());
};
} // namespace OptionalViewTests
