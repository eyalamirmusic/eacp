#pragma once

#include "Widgets.h"

#include <string>

namespace eacp::UI
{
// A row of tabs, exactly one of them selected.
//
// Drawn by this component rather than built out of Buttons. A latching Button
// keeps its own state, so a row of them is a set of independent toggles that
// have to be talked out of disagreeing -- clearing the others on every click,
// and again whenever the selection is set from outside. One component with one
// index cannot disagree with itself, and it costs a fraction of the components.
class TabBar final : public Component
{
public:
    TabBar();

    // The first tab to arrive becomes the selected one, silently: a bar showing
    // no tab at all is a state the rest of this widget has no answer for.
    void addTab(std::string name);
    void clearTabs();

    int getNumTabs() const { return names.size(); }
    const std::string& getTabName(int index) const;

    // Silent unless asked, as everywhere else in the tier. An index naming no
    // tab is ignored rather than clamped, so a caller cannot select something
    // it did not name.
    void setCurrentTabIndex(int index, bool notify = false);
    int getCurrentTabIndex() const { return currentIndex; }

    // Where a tab is drawn, in this component's own coordinates. Empty for an
    // index that names none.
    Rect getTabBounds(int index) const;

    // Which tab is at `localPosition`, or -1.
    int getTabAt(Point localPosition) const;

    void setAccentColour(const Color& colour);

    std::function<void(int)> onChange = [](int) {};

    void paint(Graphics& g) override;

    void mouseDown(const MouseEvent& event) override;
    void mouseMove(const MouseEvent& event) override;
    void mouseExit(const MouseEvent&) override;

private:
    Vector<std::string> names;
    Color accent = defaultTheme().accent;
    int currentIndex = -1;
    int hoveredIndex = -1;
};

// A TabBar along the top edge and one page under it.
//
// The pages are not owned -- they are ordinary components a caller holds, the
// way children are held everywhere in this tier -- and every one but the
// current is hidden, which is what makes the ones that are not showing cost
// nothing: a hidden subtree is stepped over by the recording walk and by the
// frame both.
class TabbedComponent final : public Component
{
public:
    TabbedComponent();

    // `page` is not owned and has to outlive this component. It is resized to
    // whatever the bar leaves, so its own bounds are ignored.
    void addTab(std::string name, Component& page);

    void setCurrentTabIndex(int index, bool notify = false);
    int getCurrentTabIndex() const { return tabs.getCurrentTabIndex(); }
    int getNumTabs() const { return tabs.getNumTabs(); }

    // The page being shown, or null while there are no tabs.
    Component* getCurrentPage() const;

    void setTabBarHeight(float newHeight);
    float getTabBarHeight() const { return tabBarHeight; }

    // For the colours and the tab names -- the bar is this component's own, so
    // its selection should be moved through setCurrentTabIndex above rather
    // than behind its back.
    TabBar& getTabBar() { return tabs; }

    // Everything the bar leaves, which is where the current page goes.
    Rect getPageArea() const;

    std::function<void(int)> onChange = [](int) {};

    void resized() override;

private:
    void showPage(int index);

    TabBar tabs;
    Vector<Component*> pages;
    float tabBarHeight = 28.f;
};
} // namespace eacp::UI
