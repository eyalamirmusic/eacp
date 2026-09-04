#include "Tabs.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
namespace
{
constexpr auto tabCorner = 5.f;

// What separates one tab from the next. Taken out of each tab rather than added
// between them, so the tabs still divide the bar exactly.
constexpr auto tabGap = 1.f;

const std::string& emptyName()
{
    static const auto none = std::string {};
    return none;
}
} // namespace

TabBar::TabBar()
{
    setInterceptsMouseClicks(true);
    setMouseCursor(eacp::Graphics::MouseCursor::PointingHand);
}

void TabBar::addTab(std::string name)
{
    names.add(std::move(name));

    if (currentIndex < 0)
        currentIndex = 0;

    repaint();
}

void TabBar::clearTabs()
{
    names.clear();
    currentIndex = -1;
    hoveredIndex = -1;

    repaint();
}

const std::string& TabBar::getTabName(int index) const
{
    if (index < 0 || index >= names.size())
        return emptyName();

    return names[index];
}

void TabBar::setCurrentTabIndex(int index, bool notify)
{
    if (index < 0 || index >= names.size() || index == currentIndex)
        return;

    currentIndex = index;
    repaint();

    if (notify)
        onChange(currentIndex);
}

void TabBar::setAccentColour(const Color& colour)
{
    accent = colour;
    repaint();
}

Rect TabBar::getTabBounds(int index) const
{
    if (index < 0 || index >= names.size())
        return {};

    auto bounds = getLocalBounds();
    auto width = bounds.w / (float) names.size();

    return {(float) index * width, 0.f, width, bounds.h};
}

int TabBar::getTabAt(Point localPosition) const
{
    auto bounds = getLocalBounds();

    if (names.empty() || bounds.w <= 0.f || !bounds.contains(localPosition))
        return -1;

    auto width = bounds.w / (float) names.size();
    auto index = (int) std::floor(localPosition.x / width);

    return std::clamp(index, 0, names.size() - 1);
}

void TabBar::mouseDown(const MouseEvent& event)
{
    setCurrentTabIndex(getTabAt(event.position), true);
}

void TabBar::mouseMove(const MouseEvent& event)
{
    auto index = getTabAt(event.position);

    if (hoveredIndex == index)
        return;

    hoveredIndex = index;
    repaint();
}

void TabBar::mouseExit(const MouseEvent&)
{
    if (hoveredIndex < 0)
        return;

    hoveredIndex = -1;
    repaint();
}

void TabBar::paint(Graphics& g)
{
    const auto& theme = defaultTheme();

    for (auto index = 0; index < names.size(); ++index)
    {
        auto bounds = getTabBounds(index).inset(tabGap, 0.f);
        auto current = index == currentIndex;

        g.setColour(current ? accent : theme.panel);
        g.fillRoundedRect(bounds, tabCorner);

        if (!current && index == hoveredIndex)
        {
            g.setColour(theme.hover);
            g.fillRoundedRect(bounds, tabCorner);
        }

        g.setColour(theme.outline);
        g.drawRoundedRect(bounds, tabCorner);

        // A lit tab carries a dark caption, so it stays legible against the
        // accent rather than sitting light-on-light -- the same bargain a
        // latching Button makes.
        g.setColour(current ? Color {0.08f, 0.09f, 0.12f, 1.f} : theme.dimText);
        g.drawText(names[index], bounds, Justification::Centred);
    }
}

TabbedComponent::TabbedComponent()
{
    addAndMakeVisible(tabs);

    tabs.onChange = [this](int index)
    {
        showPage(index);
        onChange(index);
    };
}

void TabbedComponent::addTab(std::string name, Component& page)
{
    tabs.addTab(std::move(name));
    pages.add(&page);

    // Added without being shown, because a component is visible by default and
    // every page but the current one has to be hidden -- which showPage settles
    // for the whole set rather than this one guessing.
    addChildComponent(page);

    showPage(tabs.getCurrentTabIndex());
}

void TabbedComponent::setCurrentTabIndex(int index, bool notify)
{
    if (index < 0 || index >= tabs.getNumTabs() || index == getCurrentTabIndex())
        return;

    // Through the bar rather than beside it, so there is one answer to which
    // tab is current -- and silently, the swap and the report below being this
    // component's own.
    tabs.setCurrentTabIndex(index);
    showPage(index);

    if (notify)
        onChange(index);
}

Component* TabbedComponent::getCurrentPage() const
{
    auto index = getCurrentTabIndex();

    if (index < 0 || index >= pages.size())
        return nullptr;

    return pages[index];
}

void TabbedComponent::setTabBarHeight(float newHeight)
{
    tabBarHeight = newHeight;
    resized();
}

Rect TabbedComponent::getPageArea() const
{
    auto area = getLocalBounds();
    area.removeFromTop(tabBarHeight);

    return area;
}

void TabbedComponent::showPage(int index)
{
    for (auto i = 0; i < pages.size(); ++i)
    {
        auto shown = i == index;

        if (shown)
            pages[i]->setBounds(getPageArea());

        pages[i]->setVisible(shown);
    }
}

void TabbedComponent::resized()
{
    tabs.setBounds(getLocalBounds().withHeight(tabBarHeight));

    if (auto* page = getCurrentPage())
        page->setBounds(getPageArea());
}
} // namespace eacp::UI
