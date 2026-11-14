#pragma once

#include "PopupMenu.h"

#include <eacp/Graphics/Menu/Menu.h>

namespace eacp::UI
{
// A row of menu titles, each opening its menu as a PopupMenu: a menu bar drawn
// in the tree. It is what setApplicationMenuBar shows on Linux, and any tree
// can have one of its own.
//
// Each menu is turned into a PopupMenu as it opens, so its items' isEnabled and
// isChecked are asked then, as a native bar asks them; a responder-selector
// item has nothing to send to and is left out. Shortcuts are drawn as
// acceleratorText prints them ("Ctrl+S") and not bound here: on Linux the
// window binds them.
//
// A click on a title opens its menu. While one is open, the pointer crossing
// another title opens that one, Left and Right step to the neighbours, and
// Escape or a click anywhere else closes it. The menu opens over the whole
// tree, as every PopupMenu does, so a tree only as tall as the bar has to grow
// while it is open: onOpenMenuChanged is where to do that.
class MenuBarComponent final : public Component
{
public:
    struct Style
    {
        float height = 28.f;

        // Each side of a title, and the bar's own edge before the first one.
        float titlePadding = 10.f;
        float edgeInset = 4.f;

        // How far the highlight behind an open or hovered title is inset.
        float highlightInset = 2.f;
        float highlightCornerRadius = 3.f;
    };

    MenuBarComponent();

    // Closes an open menu first, which onOpenMenuChanged hears.
    void setMenuBar(eacp::Graphics::MenuBar newBar);

    const eacp::Graphics::MenuBar& getMenuBar() const;

    int getNumMenus() const;

    // In this component's coordinates. Empty for an index that names none.
    Rect getTitleBounds(int index) const;
    int getTitleAt(Point localPosition) const;

    // Opens the menu at `index`, closing the open one if it is another. False
    // for an index naming no menu and for a menu with nothing in it.
    bool openMenu(int index);
    void closeMenu();

    // -1 while no menu is open.
    int getOpenMenuIndex() const;
    const PopupMenu& getOpenMenu() const;

    // The index of the menu about to open, before it is placed, each time one
    // does - moving from one menu to another included - and -1 once the last
    // has closed.
    std::function<void(int index)> onOpenMenuChanged = [](int) {};

    Style style;

    // How the menus look. Their onPointerOutside and onSideStep are the bar's
    // and are set over whatever is here.
    PopupMenu::Options menuOptions;

    // The PopupMenu `menu` opens as, built now.
    static PopupMenu toPopupMenu(const eacp::Graphics::Menu& menu);

    void paint(Graphics& g) override;

    void mouseDown(const MouseEvent& event) override;
    void mouseMove(const MouseEvent& event) override;
    void mouseExit(const MouseEvent&) override;

private:
    float titleWidth(int index) const;
    void menuClosed();
    void pointerOutside(Point rootPosition);
    void step(int direction);

    eacp::Graphics::MenuBar bar;
    PopupMenu popup;
    int openIndex = -1;
    int hoveredIndex = -1;
};
} // namespace eacp::UI
