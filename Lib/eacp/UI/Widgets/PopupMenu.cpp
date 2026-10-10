#include "PopupMenu.h"

#include "../Host/ComponentHost.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
namespace
{
// The gap a panel leaves between itself and an anchor with an area. One opened
// at a point has none, its corner being the point.
constexpr auto anchorGap = 2.f;

// How far a press has to move before it is a drag scrolling the panel rather
// than a click on the row it started on.
constexpr auto dragSlop = 4.f;

// Points a second, while the pointer rests on a scroll arrow.
constexpr auto arrowScrollSpeed = 300.f;

// Lines from a notched wheel, points from a trackpad -- the conversion
// ComboBox and ScrollPanel make.
constexpr auto wheelLine = 40.f;

bool isWithinTree(const Component& tree, const Component* wanted)
{
    if (&tree == wanted)
        return true;

    for (auto* child: tree.getChildren())
        if (isWithinTree(*child, wanted))
            return true;

    return false;
}

// Two strokes, as Checkbox draws its tick and for its reason: two quads in the
// batch the tree is already drawing, where a path would be a mask per menu.
void drawTick(Graphics& g, const Rect& column, float size)
{
    auto box = Rect {column.center().x - size * 0.5f,
                     column.center().y - size * 0.5f,
                     size,
                     size};

    auto left = Point {box.x + size * 0.12f, box.y + size * 0.52f};
    auto middle = Point {box.x + size * 0.40f, box.y + size * 0.80f};
    auto right = Point {box.right() - size * 0.10f, box.y + size * 0.18f};

    g.drawLine(left, middle, 1.75f);
    g.drawLine(middle, right, 1.75f);
}

// A chevron pointing right, up or down, by the sign of `direction` along the
// axis it is given.
void drawChevron(Graphics& g, Point centre, float reach, Point direction)
{
    auto tip = Point {centre.x + direction.x * reach * 0.5f,
                      centre.y + direction.y * reach * 0.5f};

    auto across = Point {direction.y, direction.x};

    auto back = Point {centre.x - direction.x * reach * 0.5f,
                       centre.y - direction.y * reach * 0.5f};

    g.drawLine({back.x + across.x * reach, back.y + across.y * reach}, tip, 1.5f);
    g.drawLine({back.x - across.x * reach, back.y - across.y * reach}, tip, 1.5f);
}
} // namespace

PopupMenuStyle PopupMenuStyle::fromTheme(const Theme& theme)
{
    auto style = PopupMenuStyle {};

    style.background = theme.panel;
    style.outline = theme.outline;
    style.text = theme.text;
    style.disabledText = theme.dimText.withAlpha(0.55f);
    style.highlight = theme.accentDim;
    style.highlightedText = theme.text;
    style.separator = theme.outline;
    style.tick = theme.accent;
    style.headerText = theme.dimText;
    style.arrow = theme.dimText;

    return style;
}

// The open menu: one component over the whole root, drawing every open panel.
//
// One component rather than one per panel, so a submenu has nothing of its own
// to keep in step with the menu it came from -- no order among siblings, no
// second capture, no focus to hand between them. The cost is that the panels
// overlap within one component's drawing, which paintOver answers per panel.
class PopupMenu::Overlay final : public Component
{
public:
    Overlay(PopupMenu& ownerToUse, const Options& optionsToUse)
        : owner(ownerToUse)
        , options(optionsToUse)
    {
        setInterceptsMouseClicks(true);
        setWantsKeyboardFocus(true);
    }

    void open(const Rect& anchorInRoot)
    {
        auto* parent = getParentComponent();

        if (parent == nullptr)
            return;

        // The whole root, so a click anywhere outside the panels is still a click
        // on this component.
        setBounds(parent->getLocalBounds());

        auto panel = Panel {};
        panel.menu = &owner;
        panel.bounds = place(owner,
                             anchorInRoot,
                             options.placement,
                             options.minimumWidth,
                             anchorInRoot.isEmpty() ? 0.f : anchorGap);

        panels.add(panel);
        repaint();
    }

    Component* previousFocus = nullptr;

    int getNumPanels() const { return panels.size(); }

    Rect getPanelBounds(int depth) const
    {
        return isOpen(depth) ? panels[depth].bounds : Rect {};
    }

    int getHighlighted(int depth) const
    {
        return isOpen(depth) ? panels[depth].highlighted : -1;
    }

    float getScroll(int depth) const
    {
        return isOpen(depth) ? panels[depth].scroll : 0.f;
    }

    Rect getItemBounds(int depth, int index) const
    {
        if (!isOpen(depth) || index < 0 || index >= itemsOf(depth).size())
            return {};

        auto rows = rowArea(depth);

        return {rows.x,
                rows.y + itemTop(depth, index) - panels[depth].scroll,
                rows.w,
                itemHeight(itemsOf(depth)[index])};
    }

    void paint(Graphics& g) override
    {
        if (options.style.font.has_value())
            g.setFont(*options.style.font);

        for (auto depth = 0; depth < panels.size(); ++depth)
            paintPanel(g, depth);
    }

    // Every point in the tree, as ComboBox's list answers: a click outside the
    // panels has to close them wherever the root has since grown to.
    bool hitTest(Point) const override { return true; }

    void mouseMove(const MouseEvent& event) override { hover(event.position); }

    void mouseExit(const MouseEvent&) override
    {
        arrowScroll = {};
        pending = {};

        if (!panels.empty())
            setHighlighted(panels.size() - 1, -1, false);
    }

    void mouseDown(const MouseEvent& event) override
    {
        press = {};

        auto depth = panelAt(event.position);

        if (depth < 0)
        {
            // The last thing this branch does: closing destroys this object.
            owner.close(0, {}, true);
            return;
        }

        if (auto direction = scrollArrowAt(depth, event.position); direction != 0)
        {
            scrollBy(depth, (float) direction * options.style.itemHeight);
            return;
        }

        press.depth = depth;
        press.row = rowAt(depth, event.position);
        press.origin = event.position;
        press.startScroll = panels[depth].scroll;
    }

    void mouseDrag(const MouseEvent& event) override
    {
        if (press.depth < 0 || !isOpen(press.depth))
            return;

        auto moved = event.position.y - press.origin.y;

        if (!press.dragged && std::abs(moved) < dragSlop)
            return;

        if (maximumScroll(press.depth) <= 0.f)
            return;

        press.dragged = true;
        setScroll(press.depth, press.startScroll - moved);
    }

    void mouseUp(const MouseEvent& event) override
    {
        auto released = press;
        press = {};

        if (released.depth < 0 || released.dragged || !isOpen(released.depth))
            return;

        auto row = rowAt(released.depth, event.position);

        if (row < 0 || row != released.row)
            return;

        // Last: choosing may destroy this object.
        activate(released.depth, row, false);
    }

    bool mouseWheelMove(const MouseEvent& event) override
    {
        auto depth = panelAt(event.position);

        if (depth >= 0)
        {
            auto step = event.preciseWheel ? event.wheelDelta.y
                                           : event.wheelDelta.y * wheelLine;

            setScroll(depth, panels[depth].scroll - step);
        }

        // Taken either way: what is under an open menu does not scroll.
        return true;
    }

    // Up and Down move through the deepest panel, past what cannot be chosen;
    // Home and End go to either end of it. Right and Return open a submenu,
    // Left and Escape close one, and Escape or Back on the menu itself closes
    // it. Return takes the highlighted item.
    bool keyDown(const KeyEvent& event) override
    {
        if (panels.empty())
            return false;

        auto depth = panels.size() - 1;
        auto highlighted = panels[depth].highlighted;

        pending = {};

        switch (event.keyCode)
        {
            case KeyCode::UpArrow:
                stepHighlight(depth, -1);
                return true;

            case KeyCode::DownArrow:
                stepHighlight(depth, 1);
                return true;

            case KeyCode::Home:
                setHighlighted(depth, nextHighlightable(depth, -1, 1), true);
                return true;

            case KeyCode::End:
                setHighlighted(depth,
                               nextHighlightable(depth, itemsOf(depth).size(), -1),
                               true);
                return true;

            case KeyCode::RightArrow:
                if (highlighted >= 0 && isOpenable(itemsOf(depth)[highlighted]))
                    openSubMenu(depth, highlighted, true);

                return true;

            case KeyCode::LeftArrow:
                if (depth > 0)
                    closeFrom(depth);

                return true;

            case KeyCode::Return:
            case KeyCode::KeypadEnter:
            case KeyCode::Space:
                if (highlighted >= 0)
                    activate(depth, highlighted, true);

                return true;

            case KeyCode::Escape:
            case KeyCode::Back:
                if (depth > 0)
                {
                    closeFrom(depth);
                    return true;
                }

                owner.close(0, {}, true);
                return true;

            default:
                return false;
        }
    }

    // Focus taken by something else leaves a menu its keys no longer reach, so
    // it closes, and leaves the focus where it went.
    void focusLost() override { owner.close(0, {}, false); }

    bool advanceAnimation(double seconds) override
    {
        auto keepGoing = false;

        if (arrowScroll.depth >= 0 && isOpen(arrowScroll.depth))
        {
            auto before = panels[arrowScroll.depth].scroll;

            scrollBy(arrowScroll.depth,
                     (float) arrowScroll.direction * arrowScrollSpeed
                         * (float) seconds);

            keepGoing = seconds == 0.0 || panels[arrowScroll.depth].scroll != before;

            if (!keepGoing)
                arrowScroll = {};
        }

        if (pending.depth >= 0)
        {
            pending.remaining -= seconds;

            if (pending.remaining <= 0.0)
            {
                auto due = pending;
                pending = {};

                settle(due.depth, due.row);
            }
            else
            {
                keepGoing = true;
            }
        }

        return keepGoing;
    }

private:
    struct Panel
    {
        const PopupMenu* menu = nullptr;
        Rect bounds;
        float scroll = 0.f;
        int highlighted = -1;

        // The row of the panel before this one that opened it.
        int openerRow = -1;
    };

    // A submenu waiting on the pointer to rest: once the delay is up the
    // panels after `depth` close, and the item at `row` opens its own.
    struct Pending
    {
        int depth = -1;
        int row = -1;
        double remaining = 0.0;
    };

    struct ArrowScroll
    {
        int depth = -1;
        int direction = 0;
    };

    struct Press
    {
        int depth = -1;
        int row = -1;
        Point origin;
        float startScroll = 0.f;
        bool dragged = false;
    };

    static bool isOpenable(const Item& item)
    {
        return item.canHighlight() && item.hasSubMenu() && !item.subMenu->isEmpty();
    }

    bool isOpen(int depth) const { return depth >= 0 && depth < panels.size(); }

    const Vector<Item>& itemsOf(int depth) const
    {
        return panels[depth].menu->items;
    }

    const Style& style() const { return options.style; }

    float itemHeight(const Item& item) const
    {
        return item.isSeparator ? style().separatorHeight : style().itemHeight;
    }

    float itemTop(int depth, int index) const
    {
        auto top = 0.f;
        const auto& items = itemsOf(depth);

        for (auto i = 0; i < index; ++i)
            top += itemHeight(items[i]);

        return top;
    }

    float contentHeight(const PopupMenu& menu) const
    {
        auto total = 0.f;

        for (const auto& item: menu.items)
            total += itemHeight(item);

        return total;
    }

    Font fontFor(const Item& item) const
    {
        auto font = style().font.value_or(getHostFont());

        if (item.isSectionHeader)
            font.style = style().headerStyle;

        return font;
    }

    float contentWidth(const PopupMenu& menu) const
    {
        auto widestText = 0.f;
        auto widestShortcut = 0.f;
        auto anySubMenu = false;

        for (const auto& item: menu.items)
        {
            if (item.isSeparator)
                continue;

            widestText = std::max(widestText, measureText(item.text, fontFor(item)));

            if (!item.shortcutText.empty())
                widestShortcut = std::max(
                    widestShortcut, measureText(item.shortcutText, fontFor(item)));

            anySubMenu = anySubMenu || item.hasSubMenu();
        }

        auto width = style().padding * 2.f + style().textInset * 2.f
                     + style().tickWidth + widestText;

        if (widestShortcut > 0.f)
            width += style().shortcutGap + widestShortcut;

        if (anySubMenu)
            width += style().arrowWidth;

        return std::ceil(width);
    }

    // Where a panel for `menu` goes against `anchor`, in the root's space, kept
    // inside the root: the tree's surface is all the room there is, a plugin
    // editor having no screen to spill onto.
    Rect place(const PopupMenu& menu,
               const Rect& anchor,
               Placement placement,
               float minimumWidth,
               float gap) const
    {
        auto area = getLocalBounds();

        auto width = std::min(std::max(contentWidth(menu), minimumWidth), area.w);
        auto wanted = contentHeight(menu) + style().padding * 2.f;

        if (placement == Placement::Right)
        {
            auto height = std::min(wanted, area.h);

            auto rightRoom = area.right() - anchor.right();
            auto leftRoom = anchor.x - area.x;

            auto x = width <= rightRoom || rightRoom >= leftRoom ? anchor.right()
                                                                 : anchor.x - width;

            // Its first row level with the row that opened it.
            auto y = anchor.y - style().padding;

            return {std::clamp(x, area.x, area.right() - width),
                    std::clamp(y, area.y, area.bottom() - height),
                    width,
                    height};
        }

        auto below = area.bottom() - anchor.bottom() - gap;
        auto above = anchor.y - gap - area.y;

        // The preferred side unless it does not fit and the other has more
        // room, which is the whole of "opens upward near the bottom".
        auto opensDown = placement == Placement::Below
                             ? wanted <= below || below >= above
                             : !(wanted <= above || above >= below);

        auto room = std::max(0.f, opensDown ? below : above);
        auto height = std::min(wanted, room);

        return {std::clamp(anchor.x, area.x, area.right() - width),
                opensDown ? anchor.bottom() + gap : anchor.y - gap - height,
                width,
                height};
    }

    bool isScrollable(int depth) const
    {
        return contentHeight(*panels[depth].menu)
               > panels[depth].bounds.h - style().padding * 2.f + 0.5f;
    }

    // The rows' window: inside the padding, and between the scroll arrows when
    // the panel has them.
    Rect rowArea(int depth) const
    {
        auto rows = panels[depth].bounds.inset(style().padding);

        if (isScrollable(depth))
            rows = rows.inset(0.f, style().scrollArrowHeight);

        return rows;
    }

    float maximumScroll(int depth) const
    {
        return std::max(0.f, contentHeight(*panels[depth].menu) - rowArea(depth).h);
    }

    void setScroll(int depth, float offset)
    {
        auto clamped = std::clamp(offset, 0.f, maximumScroll(depth));

        if (clamped == panels[depth].scroll)
            return;

        panels[depth].scroll = clamped;

        // A submenu belongs beside the row that opened it, and that row has
        // just moved.
        closeFrom(depth + 1);
        repaint();
    }

    void scrollBy(int depth, float amount)
    {
        setScroll(depth, panels[depth].scroll + amount);
    }

    // The deepest panel under the point, since a later panel is drawn over an
    // earlier one it overlaps.
    int panelAt(Point position) const
    {
        for (auto depth = panels.size() - 1; depth >= 0; --depth)
            if (panels[depth].bounds.contains(position))
                return depth;

        return -1;
    }

    // -1 over the top arrow, 1 over the bottom one, 0 elsewhere.
    int scrollArrowAt(int depth, Point position) const
    {
        if (!isScrollable(depth))
            return 0;

        auto inner = panels[depth].bounds.inset(style().padding);

        if (inner.fromTop(style().scrollArrowHeight).contains(position))
            return -1;

        if (inner.fromBottom(style().scrollArrowHeight).contains(position))
            return 1;

        return 0;
    }

    int rowAt(int depth, Point position) const
    {
        auto rows = rowArea(depth);

        if (!rows.contains(position))
            return -1;

        auto y = position.y - rows.y + panels[depth].scroll;
        auto top = 0.f;
        const auto& items = itemsOf(depth);

        for (auto index = 0; index < items.size(); ++index)
        {
            top += itemHeight(items[index]);

            if (y < top)
                return index;
        }

        return -1;
    }

    // Scrolls the row into view when asked, which is what makes the keyboard
    // usable in a panel taller than the tree. Only as far as it has to.
    void setHighlighted(int depth, int row, bool scrollToIt)
    {
        auto& panel = panels[depth];

        if (row >= 0 && !itemsOf(depth)[row].canHighlight())
            row = -1;

        if (panel.highlighted != row)
        {
            panel.highlighted = row;
            repaint();
        }

        if (!scrollToIt || row < 0)
            return;

        auto top = itemTop(depth, row);
        auto lowest = top + itemHeight(itemsOf(depth)[row]) - rowArea(depth).h;

        setScroll(depth, lowest > top ? top : std::clamp(panel.scroll, lowest, top));
    }

    // The first row from `from` (exclusive) in `direction` the keyboard can land
    // on, or -1.
    int nextHighlightable(int depth, int from, int direction) const
    {
        const auto& items = itemsOf(depth);

        for (auto index = from + direction; index >= 0 && index < items.size();
             index += direction)
        {
            if (items[index].canHighlight())
                return index;
        }

        return -1;
    }

    // Never off either end, as ComboBox's list does not: a step past the last
    // item stays on it.
    void stepHighlight(int depth, int delta)
    {
        auto from = panels[depth].highlighted;

        if (from < 0)
            from = delta > 0 ? -1 : itemsOf(depth).size();

        auto next = nextHighlightable(depth, from, delta);

        if (next >= 0)
            setHighlighted(depth, next, true);
    }

    // Closes the panel at `depth` and every one after it.
    void closeFrom(int depth)
    {
        if (depth < 1 || depth >= panels.size())
            return;

        panels.resize(depth);

        if (arrowScroll.depth >= depth)
            arrowScroll = {};

        repaint();
    }

    void openSubMenu(int depth, int row, bool highlightFirst)
    {
        closeFrom(depth + 1);
        setHighlighted(depth, row, false);

        const auto& item = itemsOf(depth)[row];

        if (!isOpenable(item))
            return;

        // Against the whole width of the panel, so the submenu sits beside it
        // rather than over its padding, and level with the row.
        auto opener = getItemBounds(depth, row);
        auto beside = panels[depth].bounds;

        auto panel = Panel {};
        panel.menu = item.subMenu.get();
        panel.openerRow = row;
        panel.bounds = place(*item.subMenu,
                             {beside.x, opener.y, beside.w, opener.h},
                             Placement::Right,
                             0.f,
                             0.f);

        panels.add(panel);

        if (highlightFirst)
            setHighlighted(depth + 1, nextHighlightable(depth + 1, -1, 1), true);

        repaint();
    }

    // What a click or Return does to a row: opens its submenu, or chooses it.
    // Choosing destroys this object, so it is the last thing done.
    void activate(int depth, int row, bool fromKeyboard)
    {
        const auto& item = itemsOf(depth)[row];

        if (!item.canHighlight())
            return;

        pending = {};

        if (item.hasSubMenu())
        {
            openSubMenu(depth, row, fromKeyboard);
            return;
        }

        auto action = item.action;
        owner.close(item.id, std::move(action), true);
    }

    void settle(int depth, int row)
    {
        if (!isOpen(depth))
            return;

        closeFrom(depth + 1);

        if (row >= 0 && isOpenable(itemsOf(depth)[row]))
            openSubMenu(depth, row, false);
    }

    void schedule(int depth, int row)
    {
        if (pending.depth == depth && pending.row == row)
            return;

        pending = {depth, row, options.subMenuDelay};

        if (options.subMenuDelay <= 0.0)
        {
            pending = {};
            settle(depth, row);
            return;
        }

        startAnimating();
    }

    void hover(Point position)
    {
        if (panels.empty())
            return;

        auto depth = panelAt(position);
        auto deepest = panels.size() - 1;

        arrowScroll = {};

        if (depth < 0)
        {
            pending = {};
            setHighlighted(deepest, -1, false);
            return;
        }

        if (auto direction = scrollArrowAt(depth, position); direction != 0)
        {
            arrowScroll = {depth, direction};
            startAnimating();
            return;
        }

        // Back inside a submenu: the rows that lead to it light up again, and a
        // close the pointer set off on its way here is called off.
        for (auto parent = 0; parent < depth; ++parent)
            setHighlighted(parent, panels[parent + 1].openerRow, false);

        if (pending.depth >= 0 && pending.depth < depth)
            pending = {};

        auto row = rowAt(depth, position);

        if (depth < deepest)
        {
            if (row >= 0 && row == panels[depth + 1].openerRow)
            {
                pending = {};
                return;
            }

            setHighlighted(depth, row, false);
            schedule(depth, row);
            return;
        }

        setHighlighted(depth, row, false);

        if (row >= 0 && isOpenable(itemsOf(depth)[row]))
            schedule(depth, row);
        else
            pending = {};
    }

    void paintPanel(Graphics& g, int depth)
    {
        // Each panel over what is under it, the tree and the panels before it
        // included: within one clip region text composites above the fills
        // whatever order they were issued in, so a caption behind a panel would
        // otherwise show through it.
        g.paintOver();

        const auto& panel = panels[depth];
        const auto& items = itemsOf(depth);
        const auto& look = style();

        g.setColour(look.background);
        g.fillRoundedRect(panel.bounds, look.cornerRadius);

        if (look.outlineThickness > 0.f)
        {
            g.setColour(look.outline);
            g.drawRoundedRect(
                panel.bounds, look.cornerRadius, look.outlineThickness);
        }

        auto rows = rowArea(depth);

        {
            auto scope = Graphics::ScopedState {g};
            g.reduceClipRegion(rows);

            auto top = rows.y - panel.scroll;

            for (auto index = 0; index < items.size(); ++index)
            {
                const auto& item = items[index];
                auto row = Rect {rows.x, top, rows.w, itemHeight(item)};

                top += row.h;

                // Only what is in the panel's window, so a menu of a few
                // hundred presets costs what its visible rows do.
                if (row.bottom() < rows.y || row.y > rows.bottom())
                    continue;

                paintItem(g, item, row, index == panel.highlighted);
            }
        }

        if (!isScrollable(depth))
            return;

        auto inner = panel.bounds.inset(look.padding);
        auto reach = look.scrollArrowHeight * 0.3f;

        g.setColour(look.arrow);

        if (panel.scroll > 0.f)
            drawChevron(g,
                        inner.fromTop(look.scrollArrowHeight).center(),
                        reach,
                        {0.f, -1.f});

        if (panel.scroll < maximumScroll(depth))
            drawChevron(g,
                        inner.fromBottom(look.scrollArrowHeight).center(),
                        reach,
                        {0.f, 1.f});
    }

    void paintItem(Graphics& g, const Item& item, const Rect& row, bool highlighted)
    {
        const auto& look = style();

        if (item.isSeparator)
        {
            auto y = row.center().y;

            g.setColour(look.separator);
            g.drawLine({row.x + look.textInset, y},
                       {row.right() - look.textInset, y});
            return;
        }

        if (highlighted)
        {
            g.setColour(look.highlight);
            g.fillRoundedRect(row, std::min(3.f, look.cornerRadius));
        }

        auto content = row.inset(look.textInset, 0.f);
        auto tickColumn = content.removeFromLeft(look.tickWidth);

        if (item.ticked)
        {
            g.setColour(item.enabled ? look.tick : look.disabledText);
            drawTick(g, tickColumn, std::min(tickColumn.w, row.h) * 0.7f);
        }

        if (item.hasSubMenu())
        {
            auto arrow = content.removeFromRight(look.arrowWidth);

            g.setColour(item.enabled ? look.arrow : look.disabledText);
            drawChevron(g, arrow.center(), 3.5f, {1.f, 0.f});
        }

        auto colour = item.isSectionHeader ? look.headerText
                      : !item.enabled      ? look.disabledText
                      : highlighted        ? look.highlightedText
                                           : look.text;

        g.setColour(colour);

        if (item.isSectionHeader)
        {
            auto scope = Graphics::ScopedState {g};

            g.setFontStyle(look.headerStyle);
            g.drawText(item.text, content);
            return;
        }

        g.drawText(item.text, content);

        if (!item.shortcutText.empty())
        {
            g.setColour(item.enabled ? look.arrow : look.disabledText);
            g.drawText(item.shortcutText, content, Justification::Right);
        }
    }

    PopupMenu& owner;
    Options options;

    Vector<Panel> panels;

    Pending pending;
    ArrowScroll arrowScroll;
    Press press;
};

PopupMenu::Item::Item() = default;
PopupMenu::Item::~Item() = default;

PopupMenu::Item::Item(const Item& other)
    : text(other.text)
    , id(other.id)
    , enabled(other.enabled)
    , ticked(other.ticked)
    , isSeparator(other.isSeparator)
    , isSectionHeader(other.isSectionHeader)
    , shortcutText(other.shortcutText)
    , subMenu(other.subMenu != nullptr ? std::make_unique<PopupMenu>(*other.subMenu)
                                       : nullptr)
    , action(other.action)
{
}

PopupMenu::Item& PopupMenu::Item::operator=(const Item& other)
{
    if (this != &other)
    {
        auto copy = Item {other};
        *this = std::move(copy);
    }

    return *this;
}

PopupMenu::Item::Item(Item&& other) noexcept = default;
PopupMenu::Item& PopupMenu::Item::operator=(Item&& other) noexcept = default;

bool PopupMenu::Item::canHighlight() const
{
    return enabled && !isSeparator && !isSectionHeader;
}

bool PopupMenu::Item::hasSubMenu() const
{
    return subMenu != nullptr;
}

PopupMenu::PopupMenu() = default;

PopupMenu::~PopupMenu()
{
    closeSilently();
}

PopupMenu::PopupMenu(const PopupMenu& other)
    : onResult(other.onResult)
    , items(other.items)
{
}

PopupMenu& PopupMenu::operator=(const PopupMenu& other)
{
    if (this != &other)
    {
        closeSilently();

        items = other.items;
        onResult = other.onResult;
    }

    return *this;
}

PopupMenu::PopupMenu(PopupMenu&& other) noexcept
    : onResult(std::move(other.onResult))
{
    other.closeSilently();
    items = std::move(other.items);
}

PopupMenu& PopupMenu::operator=(PopupMenu&& other) noexcept
{
    if (this != &other)
    {
        closeSilently();
        other.closeSilently();

        items = std::move(other.items);
        onResult = std::move(other.onResult);
    }

    return *this;
}

PopupMenu::Item&
    PopupMenu::addItem(int id, std::string text, bool enabled, bool ticked)
{
    auto item = Item {};

    item.id = id;
    item.text = std::move(text);
    item.enabled = enabled;
    item.ticked = ticked;

    return addItem(std::move(item));
}

PopupMenu::Item& PopupMenu::addItem(std::string text,
                                    std::function<void()> action,
                                    bool enabled,
                                    bool ticked)
{
    auto item = Item {};

    item.text = std::move(text);
    item.action = std::move(action);
    item.enabled = enabled;
    item.ticked = ticked;

    return addItem(std::move(item));
}

PopupMenu::Item& PopupMenu::addItem(Item item)
{
    auto& added = items.add(std::move(item));

    if (overlay != nullptr)
        overlay->repaint();

    return added;
}

PopupMenu::Item& PopupMenu::addSeparator()
{
    auto item = Item {};
    item.isSeparator = true;
    item.enabled = false;

    return addItem(std::move(item));
}

PopupMenu::Item& PopupMenu::addSectionHeader(std::string text)
{
    auto item = Item {};

    item.text = std::move(text);
    item.isSectionHeader = true;
    item.enabled = false;

    return addItem(std::move(item));
}

PopupMenu::Item&
    PopupMenu::addSubMenu(std::string text, PopupMenu subMenu, bool enabled)
{
    auto item = Item {};

    item.text = std::move(text);
    item.enabled = enabled;
    item.subMenu = std::make_unique<PopupMenu>(std::move(subMenu));

    return addItem(std::move(item));
}

void PopupMenu::clear()
{
    closeSilently();
    items.clear();
}

int PopupMenu::getNumItems() const
{
    return items.size();
}

bool PopupMenu::isEmpty() const
{
    return items.empty();
}

const PopupMenu::Item& PopupMenu::getItem(int index) const
{
    return items[index];
}

bool PopupMenu::showAt(Component& parentForTree,
                       Rect anchorInParentSpace,
                       const Options& options)
{
    close(0, {}, false);

    if (items.empty())
        return false;

    auto* host = parentForTree.getHost();

    if (host == nullptr)
        return false;

    auto* root = host->getRootComponent();

    if (root == nullptr)
        return false;

    auto origin = parentForTree.localPointToRoot(
        {anchorInParentSpace.x, anchorInParentSpace.y});

    auto anchor =
        Rect {origin.x, origin.y, anchorInParentSpace.w, anchorInParentSpace.h};

    auto* focused = host->getFocusedComponent();

    overlay = std::make_unique<Overlay>(*this, options);
    overlay->previousFocus = focused;

    // Last among the root's children, so the frame paints it after everything
    // else and a click is resolved against it first.
    root->addAndMakeVisible(*overlay);

    overlay->open(anchor);

    // So the arrows, Return and Escape reach the menu rather than whatever had
    // the keyboard when it was opened.
    overlay->grabKeyboardFocus();

    return true;
}

bool PopupMenu::showAtMouse(Component& source,
                            const MouseEvent& event,
                            const Options& options)
{
    return showAt(source, {event.position.x, event.position.y, 0.f, 0.f}, options);
}

void PopupMenu::dismiss()
{
    close(0, {}, true);
}

bool PopupMenu::isShowing() const
{
    return overlay != nullptr;
}

void PopupMenu::close(int result, std::function<void()> action, bool restoreFocus)
{
    if (overlay == nullptr)
        return;

    auto* host = overlay->getHost();
    auto* previous = overlay->previousFocus;

    // Destroyed while it is still a child of the root, as ComboBox's list is:
    // a component tells the host it is going away by walking up to it, and one
    // taken off the root first would leave the host holding a dead pointer as
    // its hover or its press capture.
    overlay.reset();

    // Only to a component still in the tree, which is found by looking rather
    // than by asking it: one destroyed while the menu was open cannot be asked.
    if (restoreFocus && host != nullptr && previous != nullptr)
    {
        auto* root = host->getRootComponent();

        if (root != nullptr && isWithinTree(*root, previous))
            host->setFocusedComponent(previous);
    }

    auto respond = onResult;

    if (action)
        action();

    if (respond)
        respond(result);
}

void PopupMenu::closeSilently()
{
    overlay.reset();
}

int PopupMenu::getNumOpenPanels() const
{
    return overlay != nullptr ? overlay->getNumPanels() : 0;
}

Rect PopupMenu::getPanelBounds(int depth) const
{
    return overlay != nullptr ? overlay->getPanelBounds(depth) : Rect {};
}

Rect PopupMenu::getItemBounds(int depth, int index) const
{
    return overlay != nullptr ? overlay->getItemBounds(depth, index) : Rect {};
}

int PopupMenu::getHighlightedIndex(int depth) const
{
    return overlay != nullptr ? overlay->getHighlighted(depth) : -1;
}

float PopupMenu::getScrollPosition(int depth) const
{
    return overlay != nullptr ? overlay->getScroll(depth) : 0.f;
}
} // namespace eacp::UI
