#include "MenuBarComponent.h"

#include "../Component/SafePointer.h"

#include <eacp/Graphics/Menu/MenuCommands.h>

#include <cmath>

namespace eacp::UI
{
namespace GraphicsMenu = eacp::Graphics;

MenuBarComponent::MenuBarComponent()
{
    setInterceptsMouseClicks(true);
}

void MenuBarComponent::setMenuBar(GraphicsMenu::MenuBar newBar)
{
    closeMenu();

    bar = std::move(newBar);
    hoveredIndex = -1;
    repaint();
}

const GraphicsMenu::MenuBar& MenuBarComponent::getMenuBar() const
{
    return bar;
}

int MenuBarComponent::getNumMenus() const
{
    return bar.menus.size();
}

float MenuBarComponent::titleWidth(int index) const
{
    auto font = menuOptions.style.font.value_or(getHostFont());
    auto text = measureText(bar.menus[index].title, font);

    return std::ceil(text + style.titlePadding * 2.f);
}

Rect MenuBarComponent::getTitleBounds(int index) const
{
    if (index < 0 || index >= getNumMenus())
        return {};

    auto x = style.edgeInset;

    for (auto i = 0; i < index; ++i)
        x += titleWidth(i);

    return {x, 0.f, titleWidth(index), getHeight()};
}

int MenuBarComponent::getTitleAt(Point localPosition) const
{
    for (auto index = 0; index < getNumMenus(); ++index)
        if (getTitleBounds(index).contains(localPosition))
            return index;

    return -1;
}

bool MenuBarComponent::openMenu(int index)
{
    if (index < 0 || index >= getNumMenus())
        return false;

    if (index == openIndex)
        return true;

    auto next = toPopupMenu(bar.menus[index]);

    if (next.isEmpty())
        return false;

    // Before the menu is placed: a host growing its tree for it has to have
    // grown by then.
    onOpenMenuChanged(index);

    // Closes the menu that was open without a result, so moving between menus
    // is not a close.
    popup = std::move(next);

    // The item's action runs first, and may have destroyed this bar.
    popup.onResult = [self = SafePointer<MenuBarComponent>(this)](int)
    {
        if (auto* alive = self.get())
            alive->menuClosed();
    };

    openIndex = index;
    repaint();

    auto options = menuOptions;
    options.onPointerOutside = [this](Point position) { pointerOutside(position); };
    options.onSideStep = [this](int direction) { step(direction); };

    if (!popup.showAt(*this, getTitleBounds(index), options))
    {
        menuClosed();
        return false;
    }

    return true;
}

void MenuBarComponent::closeMenu()
{
    if (popup.isShowing())
        popup.dismiss();
}

void MenuBarComponent::menuClosed()
{
    openIndex = -1;
    repaint();

    // Last: the host may relayout around this, or destroy the bar.
    onOpenMenuChanged(-1);
}

int MenuBarComponent::getOpenMenuIndex() const
{
    return openIndex;
}

const PopupMenu& MenuBarComponent::getOpenMenu() const
{
    return popup;
}

void MenuBarComponent::pointerOutside(Point rootPosition)
{
    auto index = getTitleAt(rootPointToLocal(rootPosition));

    if (index >= 0 && index != openIndex)
        openMenu(index);
}

void MenuBarComponent::step(int direction)
{
    auto count = getNumMenus();

    for (auto tries = 1; tries < count; ++tries)
    {
        auto index = ((openIndex + direction * tries) % count + count) % count;

        if (openMenu(index))
            return;
    }
}

PopupMenu MenuBarComponent::toPopupMenu(const GraphicsMenu::Menu& menu)
{
    auto popup = PopupMenu {};
    auto separatorOwed = false;

    for (const auto& item: menu.items)
    {
        auto kind = GraphicsMenu::classifyMenuEntry(item);

        if (kind == GraphicsMenu::MenuEntryKind::Skipped)
            continue;

        if (kind == GraphicsMenu::MenuEntryKind::Separator)
        {
            separatorOwed = !popup.isEmpty();
            continue;
        }

        auto subMenu = PopupMenu {};

        if (kind == GraphicsMenu::MenuEntryKind::Submenu)
        {
            subMenu = toPopupMenu(*item.submenu);

            if (subMenu.isEmpty())
                continue;
        }

        if (separatorOwed)
            popup.addSeparator();

        separatorOwed = false;

        if (kind == GraphicsMenu::MenuEntryKind::Submenu)
        {
            popup.addSubMenu(item.title, std::move(subMenu));
            continue;
        }

        auto enabled = !item.isEnabled || item.isEnabled();
        auto ticked = item.isChecked && item.isChecked();

        auto& added = popup.addItem(item.title, item.action, enabled, ticked);

        if (item.shortcut)
            added.shortcutText = GraphicsMenu::acceleratorText(*item.shortcut);
    }

    return popup;
}

void MenuBarComponent::paint(Graphics& g)
{
    const auto& theme = getHostTheme();
    auto bounds = getLocalBounds();

    g.setColour(theme.get(ThemeKeys::panel));
    g.fillRect(bounds);

    g.setColour(theme.get(ThemeKeys::outline));
    g.drawLine({bounds.x, bounds.bottom() - 0.5f},
               {bounds.right(), bounds.bottom() - 0.5f});

    if (menuOptions.style.font.has_value())
        g.setFont(*menuOptions.style.font);

    for (auto index = 0; index < getNumMenus(); ++index)
    {
        auto title = getTitleBounds(index);

        if (index == openIndex || (openIndex < 0 && index == hoveredIndex))
        {
            g.setColour(theme.get(index == openIndex ? ThemeKeys::accentDim
                                                     : ThemeKeys::hover));
            g.fillRoundedRect(title.inset(style.highlightInset),
                              style.highlightCornerRadius);
        }

        g.setColour(theme.get(ThemeKeys::text));
        g.drawText(bar.menus[index].title, title, Justification::Centred);
    }
}

void MenuBarComponent::mouseDown(const MouseEvent& event)
{
    auto index = getTitleAt(event.position);

    if (index < 0)
        return;

    if (index == openIndex)
        closeMenu();
    else
        openMenu(index);
}

void MenuBarComponent::mouseMove(const MouseEvent& event)
{
    auto index = getTitleAt(event.position);

    if (index == hoveredIndex)
        return;

    hoveredIndex = index;
    repaint();
}

void MenuBarComponent::mouseExit(const MouseEvent&)
{
    hoveredIndex = -1;
    repaint();
}
} // namespace eacp::UI
