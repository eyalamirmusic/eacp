#include "ComboBox.h"

#include "../Host/ComponentHost.h"

#include <algorithm>
#include <cmath>

namespace eacp::UI
{
namespace
{
constexpr auto boxCorner = 5.f;
constexpr auto listCorner = 5.f;

// The border the list draws around its rows, and the gap it leaves between
// itself and the box it belongs to.
constexpr auto listPadding = 4.f;
constexpr auto listGap = 2.f;

// What a row of text is indented by, on both the box and the list, so the
// selected item does not appear to move when the list opens over it.
constexpr auto textInset = 8.f;

const std::string& emptyText()
{
    static const auto none = std::string {};
    return none;
}
} // namespace

ComboBox::ComboBox(std::string placeholderToUse)
    : placeholder(std::move(placeholderToUse))
{
    setInterceptsMouseClicks(true);
    setWantsKeyboardFocus(true);
    setMouseCursor(eacp::Graphics::MouseCursor::PointingHand);
}

ComboBox::~ComboBox()
{
    hidePopup();
}

void ComboBox::addItem(std::string text)
{
    items.add(std::move(text));
    repaint();
}

void ComboBox::addItems(const Vector<std::string>& textsToAdd)
{
    for (const auto& text: textsToAdd)
        items.add(text);

    repaint();
}

void ComboBox::clear()
{
    hidePopup();

    items.clear();
    selectedIndex = -1;

    repaint();
}

const std::string& ComboBox::getItemText(int index) const
{
    if (index < 0 || index >= items.size())
        return emptyText();

    return items[index];
}

const std::string& ComboBox::getText() const
{
    return selectedIndex >= 0 ? items[selectedIndex] : placeholder;
}

void ComboBox::setSelectedIndex(int index, bool notify)
{
    if (index < -1 || index >= items.size() || index == selectedIndex)
        return;

    selectedIndex = index;
    repaint();

    if (popup.has_value())
        popup->repaint();

    if (notify)
        onChange(selectedIndex);
}

void ComboBox::setPlaceholder(std::string newPlaceholder)
{
    placeholder = std::move(newPlaceholder);
    repaint();
}

void ComboBox::setAccentColour(const Color& colour)
{
    accent = colour;
    repaint();
}

void ComboBox::showPopup()
{
    if (popup.has_value() || items.empty())
        return;

    auto* host = getHost();

    if (host == nullptr)
        return;

    auto* root = host->getRootComponent();

    if (root == nullptr)
        return;

    popup.emplace(*this);

    // Last among the root's children, so the frame paints it after everything
    // else and a click is resolved against it first -- the two orders being the
    // same walk, which is what keeps a menu from drawing over what it does not
    // also intercept.
    root->addAndMakeVisible(*popup);

    popup->layout();
    popup->setHighlightedRow(selectedIndex >= 0 ? selectedIndex : 0);

    // So Up, Down, Return and Escape reach the box rather than whatever held
    // the keyboard when the list was opened with the pointer.
    grabKeyboardFocus();

    repaint();
}

void ComboBox::hidePopup()
{
    if (!popup.has_value())
        return;

    // Destroyed while it is still a child of the root, and that order is the
    // whole reason this is not two lines: a component tells the host it is going
    // away by walking up to it, so one taken off the root first would never
    // reach it -- leaving the host holding a dead pointer as its hover or its
    // press capture. Component's own destructor does the removing.
    popup.reset();

    repaint();
}

Rect ComboBox::getPopupBounds() const
{
    return popup.has_value() ? popup->getListBounds() : Rect {};
}

void ComboBox::itemChosen(int index)
{
    hidePopup();
    setSelectedIndex(index, true);
}

void ComboBox::stepSelection(int delta)
{
    setSelectedIndex(std::max(0, selectedIndex + delta), true);
}

void ComboBox::resized()
{
    if (popup.has_value())
        popup->layout();
}

void ComboBox::mouseEnter(const MouseEvent&)
{
    repaint();
}

void ComboBox::mouseExit(const MouseEvent&)
{
    repaint();
}

void ComboBox::mouseDown(const MouseEvent&)
{
    showPopup();
}

void ComboBox::focusLost()
{
    // A list left open behind a field that has taken the keyboard is a menu
    // nothing can dismiss, the keys that would have closed it now going
    // somewhere else.
    hidePopup();
    repaint();
}

bool ComboBox::keyDown(const KeyEvent& event)
{
    using namespace eacp::Graphics;

    if (popup.has_value())
    {
        switch (event.keyCode)
        {
            case KeyCode::UpArrow:
                popup->moveHighlight(-1);
                return true;

            case KeyCode::DownArrow:
                popup->moveHighlight(1);
                return true;

            case KeyCode::Return:
            case KeyCode::KeypadEnter:
            {
                auto row = popup->getHighlightedRow();

                if (row >= 0)
                    itemChosen(row);
                else
                    hidePopup();

                return true;
            }

            case KeyCode::Escape:
                hidePopup();
                return true;

            default:
                return false;
        }
    }

    switch (event.keyCode)
    {
        case KeyCode::UpArrow:
            stepSelection(-1);
            return true;

        case KeyCode::DownArrow:
            stepSelection(1);
            return true;

        case KeyCode::Return:
        case KeyCode::KeypadEnter:
        case KeyCode::Space:
            showPopup();
            return true;

        default:
            return false;
    }
}

void ComboBox::paint(Graphics& g)
{
    const auto& theme = defaultTheme();
    auto bounds = getLocalBounds();
    auto focused = hasKeyboardFocus();

    g.setColour(theme.panel);
    g.fillRoundedRect(bounds, boxCorner);

    if (isMouseOver() || popup.has_value())
    {
        g.setColour(theme.hover);
        g.fillRoundedRect(bounds, boxCorner);
    }

    g.setColour(focused || popup.has_value() ? accent : theme.outline);
    g.drawRoundedRect(bounds, boxCorner, focused || popup.has_value() ? 2.f : 1.f);

    // Two strokes rather than a path, for what a checkbox's tick is drawn this
    // way for: an arrow this small is two quads in the batch the tree is
    // already drawing in, where a path would be a mask in the atlas per box.
    auto arrow = bounds.fromRight(18.f);
    auto centre = arrow.center();
    auto reach = 4.f;

    g.setColour(theme.dimText);
    g.drawLine({centre.x - reach, centre.y - reach * 0.5f},
               {centre.x, centre.y + reach * 0.5f},
               1.5f);
    g.drawLine({centre.x, centre.y + reach * 0.5f},
               {centre.x + reach, centre.y - reach * 0.5f},
               1.5f);

    auto text = bounds;
    text.w -= arrow.w;

    g.setColour(selectedIndex >= 0 ? theme.text : theme.dimText);
    g.drawText(getText(), text.inset(textInset, 0.f));
}

ComboBox::Popup::Popup(ComboBox& ownerToUse)
    : owner(ownerToUse)
{
    setInterceptsMouseClicks(true);
}

void ComboBox::Popup::layout()
{
    auto* parent = getParentComponent();

    if (parent == nullptr)
        return;

    // The whole root, so that a click anywhere outside the list is still a
    // click on this component and can put it away.
    setBounds(parent->getLocalBounds());

    setScrollPosition(scrollOffset);
    repaint();
}

float ComboBox::Popup::getRowHeight() const
{
    return std::clamp(owner.getHeight(), 20.f, 30.f);
}

Rect ComboBox::Popup::getListBounds() const
{
    auto* parent = getParentComponent();

    if (parent == nullptr)
        return {};

    auto area = parent->getLocalBounds();

    auto origin = owner.localPointToRoot({});
    auto box = Rect {origin.x, origin.y, owner.getWidth(), owner.getHeight()};

    auto wanted = (float) owner.getNumItems() * getRowHeight() + listPadding * 2.f;

    auto below = area.bottom() - box.bottom() - listGap;
    auto above = box.y - listGap;

    // Downward unless it does not fit and there is more room the other way,
    // which is the whole of "opens upward near the bottom": the tree's surface
    // is all the room there is, a plugin editor having no screen to spill onto.
    auto opensDown = wanted <= below || below >= above;
    auto room = std::max(0.f, opensDown ? below : above);
    auto height = std::min(wanted, room);

    return {box.x,
            opensDown ? box.bottom() + listGap : box.y - listGap - height,
            box.w,
            height};
}

float ComboBox::Popup::maximumScroll() const
{
    auto visible = getListBounds().h - listPadding * 2.f;
    auto content = (float) owner.getNumItems() * getRowHeight();

    return std::max(0.f, content - visible);
}

void ComboBox::Popup::setScrollPosition(float newOffset)
{
    auto clamped = std::clamp(newOffset, 0.f, maximumScroll());

    if (clamped == scrollOffset)
        return;

    scrollOffset = clamped;
    repaint();
}

int ComboBox::Popup::rowAt(Point position) const
{
    auto rows = getListBounds().inset(listPadding);
    auto rowHeight = getRowHeight();

    if (rowHeight <= 0.f || !rows.contains(position))
        return -1;

    auto row = (int) std::floor((position.y - rows.y + scrollOffset) / rowHeight);

    return row >= 0 && row < owner.getNumItems() ? row : -1;
}

void ComboBox::Popup::setHighlightedRow(int row)
{
    if (highlighted != row)
    {
        highlighted = row;
        repaint();
    }

    if (row < 0)
        return;

    // Only as far as it has to be, from whichever edge the row left: a list
    // whose rows all fit never scrolls at all.
    auto rowHeight = getRowHeight();
    auto top = (float) row * rowHeight;
    auto lowest = top + rowHeight - (getListBounds().h - listPadding * 2.f);

    // A list squeezed to less than one row can only be aligned to the row's
    // top, which is also what keeps the clamp from being asked for a range that
    // runs backwards.
    setScrollPosition(lowest > top ? top : std::clamp(scrollOffset, lowest, top));
}

void ComboBox::Popup::moveHighlight(int delta)
{
    auto count = owner.getNumItems();

    if (count == 0)
        return;

    // From the selection when nothing is highlighted yet, so the first arrow
    // moves off what the box is showing rather than off the top of the list.
    auto from = highlighted >= 0 ? highlighted : owner.getSelectedIndex();

    setHighlightedRow(std::clamp(from + delta, 0, count - 1));
}

void ComboBox::Popup::mouseDown(const MouseEvent& event)
{
    auto row = rowAt(event.position);

    // Both paths destroy this popup, which is this object: nothing after them
    // may touch a member of it, so the owner is taken out first and each is the
    // last thing its branch does.
    auto& box = owner;

    if (row < 0)
    {
        box.hidePopup();
        return;
    }

    box.itemChosen(row);
}

void ComboBox::Popup::mouseMove(const MouseEvent& event)
{
    setHighlightedRow(rowAt(event.position));
}

void ComboBox::Popup::mouseExit(const MouseEvent&)
{
    setHighlightedRow(-1);
}

bool ComboBox::Popup::mouseWheelMove(const MouseEvent& event)
{
    if (maximumScroll() <= 0.f)
        return false;

    // Points from a trackpad, lines from a notched wheel -- the same conversion
    // ScrollPanel makes, and for the same reason it has to be made here.
    auto step = event.preciseWheel ? event.wheelDelta.y : event.wheelDelta.y * 40.f;

    setScrollPosition(scrollOffset - step);

    return true;
}

void ComboBox::Popup::paint(Graphics& g)
{
    // Everything below lands over the tree the list was dropped on, its glyphs
    // included: within one clip region text composites above the fills whatever
    // order the two were issued in, so a caption behind the list would
    // otherwise show through it.
    g.paintOver();

    const auto& theme = defaultTheme();

    auto list = getListBounds();
    auto rowHeight = getRowHeight();

    g.setColour(theme.panel);
    g.fillRoundedRect(list, listCorner);

    g.setColour(theme.outline);
    g.drawRoundedRect(list, listCorner);

    auto rows = list.inset(listPadding);

    auto scope = Graphics::ScopedState {g};
    g.reduceClipRegion(rows);

    for (auto index = 0; index < owner.getNumItems(); ++index)
    {
        auto row = Rect {rows.x,
                         rows.y + (float) index * rowHeight - scrollOffset,
                         rows.w,
                         rowHeight};

        // Only what is actually in the list's window. A choice parameter with a
        // few hundred values is one component either way, and this is what
        // keeps it one component's worth of drawing too.
        if (row.bottom() < rows.y || row.y > rows.bottom())
            continue;

        if (index == highlighted)
        {
            g.setColour(owner.accent.withAlpha(0.35f));
            g.fillRoundedRect(row, 3.f);
        }

        g.setColour(index == owner.getSelectedIndex() ? theme.text : theme.dimText);
        g.drawText(owner.getItemText(index),
                   row.inset(textInset - listPadding, 0.f));
    }
}
} // namespace eacp::UI
