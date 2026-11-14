#include "MenuBarComponent.h"

#include "../Host/ComponentHost.h"

#include <eacp/Graphics/Menu/LinuxMenuBar-Linux.h>

#include <algorithm>

// What draws setApplicationMenuBar's bar on Linux, registered with
// eacp-graphics when the program starts. CMake keeps this file in every link
// of eacp-ui (--undefined=eacpUILinuxMenuBar), since nothing calls into it.

namespace eacp::UI
{
namespace
{
// The gap PopupMenu leaves under a title, and room under the deepest panel so
// its outline is not cut.
constexpr auto menuBarAnchorGap = 2.f;
constexpr auto menuBarExtentMargin = 8.f;

// How far below its own top a menu reaches, its submenus included, laid out as
// PopupMenu lays panels out: a submenu's first row level with its opener.
float menuExtent(const PopupMenu& menu, const PopupMenuStyle& style)
{
    auto rowTop = style.padding;
    auto extent = 0.f;

    for (auto index = 0; index < menu.getNumItems(); ++index)
    {
        const auto& item = menu.getItem(index);

        if (item.hasSubMenu())
            extent = std::max(
                extent, rowTop - style.padding + menuExtent(*item.subMenu, style));

        rowTop += item.isSeparator ? style.separatorHeight : style.itemHeight;
    }

    return std::max(extent, rowTop + style.padding);
}

// The root of the bar's tree: the bar along its top, and while a menu is open
// the clear space the menu is drawn over.
class MenuBarRoot final : public Component
{
public:
    explicit MenuBarRoot(MenuBarComponent& barToUse)
        : bar(barToUse)
    {
        addAndMakeVisible(bar);
    }

    void resized() override
    {
        bar.setBounds({0.f, 0.f, getWidth(), bar.style.height});
    }

private:
    MenuBarComponent& bar;
};

class MenuBarPresenter final : public eacp::Graphics::LinuxMenuBarPresenter
{
public:
    MenuBarPresenter(const eacp::Graphics::MenuBar& menus,
                     Callback layoutChangedToUse)
        : layoutChanged(std::move(layoutChangedToUse))
    {
        bar.setMenuBar(menus);
        bar.onOpenMenuChanged = [this](int index)
        {
            openIndex = index;
            layoutChanged();
        };

        host.setTransparent(true);
        host.setBackgroundColour(Color::transparent());
        host.setRootComponent(root);
    }

    ~MenuBarPresenter() override
    {
        bar.onOpenMenuChanged = [](int) {};
        bar.closeMenu();
    }

    eacp::Graphics::View& getView() override { return host; }

    float getHeight() const override { return bar.style.height; }

    // Open, the tree reaches as far down as the open menu needs, which is
    // all the window shows of the tree: no more of the content is covered.
    void layout(Rect windowBounds) override
    {
        auto height = bar.style.height;

        if (openIndex >= 0)
        {
            const auto& menu = bar.getMenuBar().menus[openIndex];
            auto extent = menuExtent(MenuBarComponent::toPopupMenu(menu),
                                     bar.menuOptions.style);

            height =
                std::min(windowBounds.h,
                         height + menuBarAnchorGap + extent + menuBarExtentMargin);
        }

        host.setBounds({0.f, 0.f, windowBounds.w, std::max(height, 0.f)});
    }

    bool isMenuOpen() const override { return openIndex >= 0; }

    void dismissMenu() override { bar.closeMenu(); }

    bool keyPressed(const eacp::Graphics::KeyEvent& event) override
    {
        if (openIndex < 0)
            return false;

        host.dispatchKeyEvent(event);
        return true;
    }

private:
    Callback layoutChanged;
    MenuBarComponent bar;
    MenuBarRoot root {bar};
    ComponentHost host;
    int openIndex = -1;
};

bool registerMenuBarPresenter()
{
    eacp::Graphics::setLinuxMenuBarPresenterFactory(
        [](const eacp::Graphics::MenuBar& menus, Callback layoutChanged)
            -> std::unique_ptr<eacp::Graphics::LinuxMenuBarPresenter>
        {
            return std::make_unique<MenuBarPresenter>(menus,
                                                      std::move(layoutChanged));
        });

    return true;
}
} // namespace
} // namespace eacp::UI

extern "C" const bool eacpUILinuxMenuBar = eacp::UI::registerMenuBarPresenter();
